// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2/network.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace radioberry::daemon::p2 {
namespace {

sockaddr_in socket_address(std::uint32_t address, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(address);
    a.sin_port = htons(port);
    return a;
}

void set_int(int fd, int level, int name, int value) {
    ::setsockopt(fd, level, name, &value, sizeof value);
}

constexpr int kDscpEf = 0xb8;  // IP TOS byte: DSCP 46, expedited forwarding

}  // namespace

Network::Network(std::uint32_t address, std::uint16_t base, unsigned ddcs, SendBase send_base)
    : send_base_(std::move(send_base)) {
    std::vector<unsigned> offsets = {kRxSpecificPort, kTxSpecificPort, kHighPriorityPort,
                                     kAudioPort, kTxIqPort};
    for (unsigned d = 0; d < ddcs; ++d) {
        offsets.push_back(kDdcPort0 + d);
    }
    for (const unsigned offset : offsets) {
        const std::uint16_t want = base == 0 ? 0 : static_cast<std::uint16_t>(base + offset);
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            const int e = errno;
            close_all();
            throw std::system_error(e, std::generic_category(), "UDP socket");
        }
        set_int(fd, SOL_SOCKET, SO_SNDBUF, 1 << 20);
        set_int(fd, SOL_SOCKET, SO_RCVBUF, 1 << 20);
        set_int(fd, IPPROTO_IP, IP_TOS, kDscpEf);  // as the Protocol 1 socket
        set_int(fd, SOL_SOCKET, SO_PRIORITY, 6);
        sockaddr_in a = socket_address(address, want);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
            const int e = errno;
            ::close(fd);
            close_all();
            throw std::system_error(e, std::generic_category(),
                                    "UDP bind to port " + std::to_string(want));
        }
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        sockets_[offset] = {fd, ntohs(a.sin_port)};
    }
    fds_.push_back({wake_.fd(), POLLIN, 0});
    offsets_.push_back(0);
    for (const auto& [offset, s] : sockets_) {
        fds_.push_back({s.fd, POLLIN, 0});
        offsets_.push_back(offset);
    }
}

Network::~Network() { close_all(); }

void Network::close_all() {
    for (const auto& [offset, s] : sockets_) {
        ::close(s.fd);
    }
    sockets_.clear();
}

std::uint16_t Network::port(unsigned offset) const {
    const auto it = sockets_.find(offset);
    return it == sockets_.end() ? 0 : it->second.port;
}

void Network::poll(int timeout_ms, const Handler& handler) {
    if (::poll(fds_.data(), fds_.size(), timeout_ms) <= 0) {
        return;
    }
    if (fds_[0].revents != 0) {
        wake_.drain();
    }
    std::array<std::uint8_t, 2048> buffer;
    for (std::size_t i = 1; i < fds_.size(); ++i) {
        if (fds_[i].revents == 0) {
            continue;
        }
        for (;;) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const ssize_t got = ::recvfrom(fds_[i].fd, buffer.data(), buffer.size(), 0,
                                           reinterpret_cast<sockaddr*>(&from), &len);
            if (got < 0) {
                break;  // EAGAIN, or an error the next poll reports again
            }
            handler(offsets_[i],
                    std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(got)),
                    Peer{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), false});
        }
    }
}

void Network::send(unsigned port, const Peer& to, std::span<const std::uint8_t> bytes) {
    if (port == kGeneralPort) {
        send_base_(to, bytes);
        return;
    }
    const auto it = sockets_.find(port);
    const sockaddr_in a = socket_address(to.address, to.port);
    if (it == sockets_.end() || ::sendto(it->second.fd, bytes.data(), bytes.size(), MSG_DONTWAIT,
                                         reinterpret_cast<const sockaddr*>(&a), sizeof a) < 0) {
        const std::lock_guard lock(mutex_);
        ++send_failures_;
    }
}

std::uint64_t Network::send_failures() const {
    const std::lock_guard lock(mutex_);
    return send_failures_;
}

}  // namespace radioberry::daemon::p2
