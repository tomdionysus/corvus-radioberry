// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace radioberry::daemon {

// Where a packet came from or goes: an IPv4 address and port (host byte
// order), over UDP or over the TCP connection from that address.
struct Peer {
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    bool tcp = false;
    bool operator==(const Peer&) const = default;
};

enum class Protocol { P1, P2 };

// A client of one protocol.
struct Owner {
    Protocol protocol = Protocol::P1;
    Peer peer;
    bool operator==(const Owner&) const = default;
};

// "192.0.2.7:50000 (UDP)"
std::string describe(const Peer& peer);
// "Protocol 1 client 192.0.2.7:50000 (UDP)"
std::string describe(const Owner& owner);

// Which protocol a packet on the discovery port belongs to: Protocol 1
// packets start EF FE; a Protocol 2 discovery is 60 bytes with command
// 0x02 after a zero sequence number.
bool is_p2_discovery(std::span<const std::uint8_t> packet);

// Who owns the radio, shared by the protocol servers; thread-safe.
//
// The first client to claim it owns it until it releases it or is silent
// for the timeout; others are refused meanwhile. Losing the owner puts
// the gateware in the safe state. A stream fault while the owner holds
// MOX puts it in the safe state and latches: MOX is refused until the
// client sends MOX clear.
class Session {
public:
    using MakeSafe = std::function<void()>;

    Session(MakeSafe make_safe, std::chrono::nanoseconds timeout);

    // True if `client` owns the radio, now or already.
    bool claim(const Owner& client, std::chrono::nanoseconds now);
    bool owns(const Owner& client) const;
    // Owned by someone other than `client`.
    bool busy_for(const Owner& client) const;
    std::optional<Owner> owner() const;
    // The owner was heard from; restarts its timeout.
    void heard(const Owner& client, std::chrono::nanoseconds now);
    // The owner gives the radio up; safe state.
    void release(const Owner& client, const std::string& why);
    // Releases a silent owner; returns it.
    std::optional<Owner> tick(std::chrono::nanoseconds now);

    // A stream fault. Latches and makes safe if MOX is held; otherwise
    // only counted.
    void fault(const std::string& why);
    // The MOX the owner may have: `requested`, unless a fault is latched.
    // A request without MOX clears the latch. `apply`, if given, is
    // called with the result under the session's lock, the lock fault()
    // and the safe state are taken under, so a fault cannot fall between
    // granting MOX and acting on it.
    bool mox(bool requested, const std::function<void(bool)>& apply = {});
    bool latched() const;
    // MOX is granted now.
    bool transmitting() const;
    std::uint64_t faults() const;

private:
    void drop_locked(const std::string& why);

    MakeSafe make_safe_;
    const std::chrono::nanoseconds timeout_;
    mutable std::mutex mutex_;
    std::optional<Owner> owner_;
    std::chrono::nanoseconds last_heard_{};
    bool mox_ = false;
    bool latched_ = false;
    std::uint64_t faults_ = 0;
};

}  // namespace radioberry::daemon
