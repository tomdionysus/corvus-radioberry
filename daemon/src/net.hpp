// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>

#include "p1/server.hpp"
#include "session.hpp"

namespace radioberry::daemon {

// The Protocol 1 sockets: UDP on the port, and a TCP listener on the same
// port taking one connection at a time, which carries 1032-byte records
// both ways as piHPSDR's TCP mode does. Failures to set up throw
// std::system_error. poll() and close() run on one thread, which alone
// opens and closes the TCP connection; send() may be called from another.
class Network final : public p1::Link {
public:
    // Binds to `address` (host byte order, 0 for all) and `port`; port 0
    // takes a free one.
    Network(std::uint32_t address, std::uint16_t port);
    ~Network() override;
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    std::uint16_t port() const { return port_; }

    using Handler = std::function<void(std::span<const std::uint8_t>, const p1::Peer&)>;
    // Waits up to `timeout_ms` for traffic and hands each datagram and each
    // whole TCP record to `handler`.
    void poll(int timeout_ms, const Handler& handler);

    void send(const p1::Peer& to, std::span<const std::uint8_t> bytes) override;
    void close(const p1::Peer& peer) override;

    std::uint64_t send_failures() const;

private:
    void read_udp(const Handler& handler);
    void accept_tcp();
    void read_tcp(const Handler& handler);
    void close_tcp_locked();

    int udp_ = -1;
    int listener_ = -1;
    std::uint16_t port_ = 0;

    mutable std::mutex tcp_mutex_;  // tcp_ and tcp_peer_, for send()
    int tcp_ = -1;
    p1::Peer tcp_peer_;
    std::array<std::uint8_t, p1::kPacketBytes> record_{};
    std::size_t record_fill_ = 0;
    std::uint64_t send_failures_ = 0;
};

// The hardware address of the first running non-loopback interface, or
// nothing.
std::optional<std::array<std::uint8_t, 6>> interface_mac();

}  // namespace radioberry::daemon
