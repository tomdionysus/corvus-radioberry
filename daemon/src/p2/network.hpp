// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>

#include "p2/server.hpp"
#include "session.hpp"

namespace radioberry::daemon::p2 {

// The Protocol 2 sockets other than the base port, which the Protocol 1
// sockets own: UDP on base + 1 to base + 5 (Receiver and Transmitter
// Specific, High Priority, audio and TX I/Q in; status and Mic/Line out),
// and base + 11 onwards, one per DDC, for DDC I/Q out. Packets for the
// base port go out through `send_base`. With base 0, each socket takes a
// free port (for tests). Failures to set up throw std::system_error.
// poll() runs on one thread; send() may be called from any.
class Network final : public Link {
public:
    using SendBase = std::function<void(const Peer&, std::span<const std::uint8_t>)>;

    Network(std::uint32_t address, std::uint16_t base, unsigned ddcs, SendBase send_base);
    ~Network() override;
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    // The UDP port bound for `offset`, or 0 if none.
    std::uint16_t port(unsigned offset) const;

    using Handler = std::function<void(unsigned, std::span<const std::uint8_t>, const Peer&)>;
    // Waits up to `timeout_ms` for traffic and hands each datagram, with
    // the offset of the port it arrived on, to `handler`.
    void poll(int timeout_ms, const Handler& handler);

    void send(unsigned port, const Peer& to, std::span<const std::uint8_t> bytes) override;

    std::uint64_t send_failures() const;

private:
    void close_all();

    struct Socket {
        int fd = -1;
        std::uint16_t port = 0;
    };
    std::map<unsigned, Socket> sockets_;  // by offset
    SendBase send_base_;
    mutable std::mutex mutex_;
    std::uint64_t send_failures_ = 0;
};

}  // namespace radioberry::daemon::p2
