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
#include "tx_policy.hpp"

namespace radioberry::daemon::p1 {

using daemon::Peer;

// The transport, implemented over sockets by Network and by fakes in tests.
class Link {
public:
    virtual ~Link() = default;
    virtual void send(const Peer& to, std::span<const std::uint8_t> bytes) = 0;
    // Several packets to one peer, in order; a socket sends them in one call.
    virtual void send_batch(const Peer& to,
                            std::span<const std::span<const std::uint8_t>> packets) {
        for (const auto& p : packets) {
            send(to, p);
        }
    }
    // Ends a TCP connection (after a discovery reply, a stop or a lost
    // session); nothing for UDP.
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
// when their value changes, except the transmit controls: the owner's
// MOX, address 0x09 (drive, PA enable) and the CWX bit of address 0x0f
// go through the TxPolicy. The owner's TX samples go to the TX ring.
// Address 0x17 is set at every stream start and its TX buffer latency is
// raised to at least kMinTxLatencyMs.
class Server {
public:
    Server(Identity identity, Control& control, SpscRing<TxBlock>& tx, Session& session,
           TxPolicy& policy, std::function<GatewareStatus()> status, Link& link);

    // Address 0x0f bit 24: CWX enable, as the HL2 reads it.
    static constexpr std::uint8_t kCwConfig = 0x0f;
    static constexpr std::uint32_t kCwxEnable = 1u << 24;
    // Address 0x17: PTT hang time (bits 12..8) and TX buffer latency (bits
    // 6..0), in ms. piHPSDR does not send it, which leaves the gateware's
    // 20 ms latency; its TX samples arrive in bursts that fell up to 17.4 ms
    // behind a steady 48 ksps (measured receive-only on a Pi 5), so the
    // latency is raised to 40 ms. That and twice the bursts' 17.4 ms swing
    // stay inside the gateware's 85 ms FIFO. The hang time is the
    // gateware's default.
    static constexpr std::uint8_t kTxTiming = 0x17;
    static constexpr std::uint32_t kTxLatencyMask = 0x7f;
    static constexpr std::uint32_t kPttHangMs = 12;
    static constexpr std::uint32_t kMinTxLatencyMs = 40;

    void on_packet(std::span<const std::uint8_t> packet, const Peer& from,
                   std::chrono::nanoseconds now);
    void on_rx(const RxBlock& block);
    // Stops streaming to a client that no longer owns the radio (call
    // after Session::tick).
    void tick();

    // The radio's frequency error (positive: it tunes high); every RX and
    // TX frequency command is corrected for it.
    void set_frequency_ppm(double ppm);
    void set_iq_convention(IqConvention convention);

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
    TxPolicy& policy_;
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
    double frequency_ppm_ = 0;
    bool swap_iq_ = false;
    // Values Control refused, so each is logged once.
    std::array<std::optional<std::uint32_t>, Control::kAddresses> refused_{};
    ServerCounters counters_;

    // Used by on_rx only.
    std::vector<std::array<std::uint8_t, kPacketBytes>> outgoing_;
    std::vector<std::span<const std::uint8_t>> batch_;
};

}  // namespace radioberry::daemon::p1
