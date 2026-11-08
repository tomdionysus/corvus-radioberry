// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "device/control.hpp"
#include "device/ring.hpp"
#include "device/rx_pump.hpp"
#include "device/tx_pump.hpp"
#include "p1/protocol1.hpp"
#include "session.hpp"

namespace radioberry::daemon::p1 {

using daemon::Peer;

// The transport, implemented over sockets by Network and by fakes in tests.
class Link {
public:
    virtual ~Link() = default;
    virtual void send(const Peer& to, std::span<const std::uint8_t> bytes) = 0;
    // Ends a TCP connection (after a discovery reply or a stop); nothing
    // for UDP.
    virtual void close(const Peer& peer) = 0;
};

// What the gateware's replies show, for the response words.
struct GatewareStatus {
    bool cw_or_ptt = false;
    bool cw_key = false;
};

struct ServerCounters {
    std::uint64_t packets_in = 0;
    std::uint64_t packets_out = 0;
    std::uint64_t host_sequence_gaps = 0;
    std::uint64_t tx_blocks_dropped = 0;
    std::uint64_t ignored = 0;  // from a client not owning the radio, or malformed
};

// The Protocol 1 server. The network thread calls on_packet() and
// tick(); the RX thread calls on_rx() with every RX block.
//
// A client streams while it owns the radio in the Session; packets from
// others are ignored meanwhile. Commands are forwarded to the gateware
// when their value changes. This version does not transmit: MOX is logged
// and ignored, and address 0x09 is sent with drive, VNA and PA enable
// cleared.
class Server {
public:
    Server(Identity identity, Control& control, SpscRing<TxBlock>& tx, Session& session,
           std::function<GatewareStatus()> status, Link& link);

    void on_packet(std::span<const std::uint8_t> packet, const Peer& from,
                   std::chrono::nanoseconds now);
    void on_rx(const RxBlock& block);
    // Stops streaming to a client that no longer owns the radio (call
    // after Session::tick).
    void tick();

    bool running() const;
    std::optional<Peer> client() const;
    // Receivers the client asked for, and those the device runs.
    unsigned client_receivers() const;
    unsigned device_receivers() const;
    ServerCounters counters() const;

private:
    static Owner owner(const Peer& peer) { return {Protocol::P1, peer}; }
    void start_locked(const Peer& from);
    void apply_locked(const HostCommand& command);
    void queue_tx_locked(const HostPacket& packet);

    const Identity identity_;
    Control& control_;
    SpscRing<TxBlock>& tx_;
    Session& session_;
    std::function<GatewareStatus()> status_;
    Link& link_;

    mutable std::mutex mutex_;
    bool running_ = false;
    Peer client_;
    std::optional<std::uint32_t> last_host_sequence_;
    RxPacker packer_;
    unsigned client_receivers_ = 1;
    unsigned device_receivers_ = 1;
    unsigned status_address_ = 0;
    std::optional<HostCommand> pending_ack_;
    bool mox_warned_ = false;
    // Values Control refused, so each is logged once.
    std::array<std::optional<std::uint32_t>, Control::kAddresses> refused_{};
    ServerCounters counters_;

    // Used by on_rx only.
    std::vector<std::array<std::uint8_t, kPacketBytes>> outgoing_;
};

}  // namespace radioberry::daemon::p1
