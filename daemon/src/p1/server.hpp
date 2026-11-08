// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

namespace radioberry::daemon::p1 {

// Where a packet came from or goes: an IPv4 address and port (host byte
// order), over UDP or over the TCP connection from that address.
struct Peer {
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    bool tcp = false;
    bool operator==(const Peer&) const = default;
};

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

struct ServerOptions {
    // A streaming client silent this long loses the radio.
    std::chrono::nanoseconds client_timeout = std::chrono::seconds(3);
};

struct ServerCounters {
    std::uint64_t packets_in = 0;
    std::uint64_t packets_out = 0;
    std::uint64_t host_sequence_gaps = 0;
    std::uint64_t tx_blocks_dropped = 0;
    std::uint64_t ignored = 0;  // from a second client, or malformed
};

// The Protocol 1 server. The network thread calls on_packet() and
// tick(); the RX thread calls on_rx() with every RX block.
//
// The first client to start streaming owns the radio until it stops or
// is silent for client_timeout; packets from others are ignored meanwhile.
// Commands are forwarded to the gateware when their value changes. This
// version does not transmit: MOX is logged and ignored, and address 0x09
// is sent with drive, VNA and PA enable cleared.
class Server {
public:
    Server(Identity identity, Control& control, SpscRing<TxBlock>& tx,
           std::function<GatewareStatus()> status, Link& link, ServerOptions options = {});

    void on_packet(std::span<const std::uint8_t> packet, const Peer& from,
                   std::chrono::nanoseconds now);
    void on_rx(const RxBlock& block);
    void tick(std::chrono::nanoseconds now);

    bool running() const;
    std::optional<Peer> client() const;
    // Receivers the client asked for, and those the device runs.
    unsigned client_receivers() const;
    unsigned device_receivers() const;
    ServerCounters counters() const;

private:
    void start_locked(const Peer& from, std::chrono::nanoseconds now);
    void stop_locked(const char* why);
    void apply_locked(const HostCommand& command);
    void queue_tx_locked(const HostPacket& packet);
    bool owned_by_other_locked(const Peer& from) const;

    const Identity identity_;
    Control& control_;
    SpscRing<TxBlock>& tx_;
    std::function<GatewareStatus()> status_;
    Link& link_;
    const ServerOptions options_;

    mutable std::mutex mutex_;
    bool running_ = false;
    std::optional<Peer> client_;
    std::chrono::nanoseconds last_heard_{};
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
