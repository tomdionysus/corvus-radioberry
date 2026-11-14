// SPDX-License-Identifier: GPL-2.0-or-later
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

#include "transport.hpp"

namespace radioberry::proto {
namespace {

// A send or receive's byte count; throws std::system_error, naming `what`,
// when it failed.
std::size_t checked(ssize_t n, const char* what) {
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(), what);
    }
    return static_cast<std::size_t>(n);
}

class UdpTransport final : public Transport {
public:
    UdpTransport(const std::string& host, std::uint16_t base_port) : base_port_(base_port) {
        radio_.sin_family = AF_INET;
        if (::inet_pton(AF_INET, host.c_str(), &radio_.sin_addr) != 1) {
            throw std::runtime_error("not an IPv4 address: " + host);
        }
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "UDP socket");
        }
        const int buffer = 4 << 20;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
    }
    ~UdpTransport() override { ::close(fd_); }
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    void send(unsigned offset, std::span<const std::uint8_t> bytes) override {
        sockaddr_in to = radio_;
        to.sin_port = htons(static_cast<std::uint16_t>(base_port_ + offset));
        checked(::sendto(fd_, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&to),
                         sizeof to),
                "send");
    }

    std::optional<Datagram> receive(std::chrono::milliseconds timeout) override {
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(timeout.count())) != 1) {
            return std::nullopt;
        }
        Datagram d;
        d.bytes.resize(2048);
        sockaddr_in from{};
        socklen_t length = sizeof from;
        const std::size_t n = checked(::recvfrom(fd_, d.bytes.data(), d.bytes.size(), 0,
                                                 reinterpret_cast<sockaddr*>(&from), &length),
                                      "receive");
        const std::uint16_t port = ntohs(from.sin_port);
        if (from.sin_addr.s_addr != radio_.sin_addr.s_addr || port < base_port_) {
            d.offset = ~0u;  // not from the radio
        } else {
            d.offset = port - base_port_;
        }
        d.bytes.resize(n);
        return d;
    }

private:
    sockaddr_in radio_{};
    std::uint16_t base_port_;
    int fd_ = -1;
};

}  // namespace

std::unique_ptr<Transport> open_udp(const std::string& host, std::uint16_t base_port) {
    return std::make_unique<UdpTransport>(host, base_port);
}

}  // namespace radioberry::proto
