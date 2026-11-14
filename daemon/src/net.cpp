// SPDX-License-Identifier: GPL-2.0-or-later
#include "net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <system_error>

namespace radioberry::daemon {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

sockaddr_in socket_address(std::uint32_t address, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(address);
    a.sin_port = htons(port);
    return a;
}

p1::Peer peer_of(const sockaddr_in& a, bool tcp) {
    return {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port), tcp};
}

void set_int(int fd, int level, int name, int value) {
    ::setsockopt(fd, level, name, &value, sizeof value);
}

constexpr int kDscpEf = 0xb8;  // IP TOS byte: DSCP 46, expedited forwarding

}  // namespace

Network::Network(std::uint32_t address, std::uint16_t port) {
    udp_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (udp_ < 0) {
        fail("UDP socket");
    }
    set_int(udp_, SOL_SOCKET, SO_BROADCAST, 1);
    set_int(udp_, SOL_SOCKET, SO_SNDBUF, 1 << 20);
    set_int(udp_, SOL_SOCKET, SO_RCVBUF, 1 << 20);
    // DSCP EF, as piHPSDR marks its own: Wi-Fi sends it in the voice
    // queue. SO_PRIORITY 6 is the highest without CAP_NET_ADMIN.
    set_int(udp_, IPPROTO_IP, IP_TOS, kDscpEf);
    set_int(udp_, SOL_SOCKET, SO_PRIORITY, 6);
    sockaddr_in a = socket_address(address, port);
    if (::bind(udp_, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        const int e = errno;
        ::close(udp_);
        errno = e;
        fail("UDP bind to port " + std::to_string(port));
    }
    socklen_t len = sizeof a;
    ::getsockname(udp_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);

    listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_ < 0) {
        const int e = errno;
        ::close(udp_);
        errno = e;
        fail("TCP socket");
    }
    set_int(listener_, SOL_SOCKET, SO_REUSEADDR, 1);
    a = socket_address(address, port_);
    if (::bind(listener_, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 ||
        ::listen(listener_, 4) != 0) {
        const int e = errno;
        ::close(udp_);
        ::close(listener_);
        errno = e;
        fail("TCP bind to port " + std::to_string(port_));
    }
}

Network::~Network() {
    {
        const std::lock_guard lock(tcp_mutex_);
        close_tcp_locked();
    }
    ::close(listener_);
    ::close(udp_);
}

void Network::poll(int timeout_ms, const Handler& handler) {
    pollfd fds[4] = {
        {wake_.fd(), POLLIN, 0}, {udp_, POLLIN, 0}, {listener_, POLLIN, 0}, {tcp_, POLLIN, 0}};
    const nfds_t n = tcp_ >= 0 ? 4 : 3;
    if (::poll(fds, n, timeout_ms) <= 0) {
        return;
    }
    if (fds[0].revents != 0) {
        wake_.drain();
    }
    if (fds[1].revents != 0) {
        read_udp(handler);
    }
    if (fds[2].revents != 0) {
        accept_tcp();
    }
    if (n == 4 && fds[3].revents != 0) {
        read_tcp(handler);
    }
}

void Network::read_udp(const Handler& handler) {
    std::array<std::uint8_t, 2048> buffer;
    for (;;) {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        const ssize_t got = ::recvfrom(udp_, buffer.data(), buffer.size(), 0,
                                       reinterpret_cast<sockaddr*>(&from), &len);
        if (got < 0) {
            return;  // EAGAIN, or an error the next poll reports again
        }
        handler(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(got)),
                peer_of(from, false));
    }
}

void Network::accept_tcp() {
    sockaddr_in from{};
    socklen_t len = sizeof from;
    const int fd = ::accept4(listener_, reinterpret_cast<sockaddr*>(&from), &len, SOCK_CLOEXEC);
    if (fd < 0) {
        return;
    }
    const std::lock_guard lock(tcp_mutex_);
    if (tcp_ >= 0) {
        char address[INET_ADDRSTRLEN] = "?";
        ::inet_ntop(AF_INET, &from.sin_addr, address, sizeof address);
        spdlog::warn("TCP connection from {} refused: one is already open", address);
        ::close(fd);
        return;
    }
    set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    const timeval timeout{0, 200000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    tcp_ = fd;
    tcp_peer_ = peer_of(from, true);
    record_fill_ = 0;
}

void Network::read_tcp(const Handler& handler) {
    const ssize_t got =
        ::recv(tcp_, record_.data() + record_fill_, record_.size() - record_fill_, MSG_DONTWAIT);
    if (got <= 0) {
        // The end of the stream or an error closes it; EAGAIN and EINTR
        // wait for the next poll.
        if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
            const std::lock_guard lock(tcp_mutex_);
            close_tcp_locked();
        }
        return;
    }
    record_fill_ += static_cast<std::size_t>(got);
    if (record_fill_ == record_.size()) {
        record_fill_ = 0;
        handler(record_, tcp_peer_);
    }
}

void Network::send(const p1::Peer& to, std::span<const std::uint8_t> bytes) {
    if (!to.tcp) {
        const sockaddr_in a = socket_address(to.address, to.port);
        if (::sendto(udp_, bytes.data(), bytes.size(), MSG_DONTWAIT,
                     reinterpret_cast<const sockaddr*>(&a), sizeof a) < 0) {
            const std::lock_guard lock(tcp_mutex_);
            ++send_failures_;
        }
        return;
    }
    const std::lock_guard lock(tcp_mutex_);
    if (tcp_ < 0 || !(tcp_peer_ == to)) {
        ++send_failures_;
        return;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::send(tcp_, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
        if (n <= 0) {
            // A part-sent record breaks the stream. The poll thread sees
            // the shutdown and closes the socket; only it closes.
            ++send_failures_;
            ::shutdown(tcp_, SHUT_RDWR);
            return;
        }
        done += static_cast<std::size_t>(n);
    }
}

void Network::close(const p1::Peer& peer) {
    const std::lock_guard lock(tcp_mutex_);
    if (peer.tcp && tcp_peer_ == peer) {
        close_tcp_locked();
    }
}

std::uint64_t Network::send_failures() const {
    const std::lock_guard lock(tcp_mutex_);
    return send_failures_;
}

void Network::close_tcp_locked() {
    if (tcp_ >= 0) {
        ::close(tcp_);
        tcp_ = -1;
        tcp_peer_ = {};
        record_fill_ = 0;
    }
}

std::optional<std::array<std::uint8_t, 6>> interface_mac() {
    ifaddrs* list = nullptr;
    const bool listed = ::getifaddrs(&list) == 0;  // none found if it fails
    std::optional<std::array<std::uint8_t, 6>> mac;
    for (const ifaddrs* i = listed ? list : nullptr; i != nullptr && !mac; i = i->ifa_next) {
        if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_PACKET ||
            (i->ifa_flags & IFF_LOOPBACK) != 0 || (i->ifa_flags & IFF_RUNNING) == 0) {
            continue;
        }
        const auto* ll = reinterpret_cast<const sockaddr_ll*>(i->ifa_addr);
        if (ll->sll_halen == 6) {
            std::array<std::uint8_t, 6> m{};
            std::copy_n(ll->sll_addr, 6, m.begin());
            mac = m;
        }
    }
    if (listed) {
        ::freeifaddrs(list);
    }
    return mac;
}

}  // namespace radioberry::daemon
