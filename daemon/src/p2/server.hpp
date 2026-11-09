// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "device/control.hpp"
#include "device/ring.hpp"
#include "device/rx_pump.hpp"
#include "device/tx_pump.hpp"
#include "dsp/decimator.hpp"
#include "p1/protocol1.hpp"
#include "p1/server.hpp"
#include "p2/protocol2.hpp"
#include "session.hpp"
#include "tx_policy.hpp"

namespace radioberry::daemon::p2 {

// The transport: each packet goes out from one of the radio's ports, by
// offset from the base port, since clients tell the streams apart by it.
class Link {
public:
    virtual ~Link() = default;
    virtual void send(unsigned port, const Peer& to, std::span<const std::uint8_t> bytes) = 0;
};

struct ServerCounters {
    std::uint64_t packets_in = 0;
    std::uint64_t ddc_packets_out = 0;
    std::uint64_t tx_iq_sequence_gaps = 0;
    std::uint64_t command_sequence_gaps = 0;  // General, Specific, High Priority
    std::uint64_t tx_blocks_dropped = 0;
    std::uint64_t ignored = 0;  // from a client not owning the radio, or malformed
};

// The Protocol 2 server. The network threads call on_packet() and tick();
// the RX thread calls on_rx() with every RX block.
//
// A client claims the radio with a General packet, or High Priority with
// run set, and releases it with run clear; others are ignored meanwhile.
// DDC n is gateware receiver n. The gateware runs every receiver at one
// rate, the highest any enabled DDC asks for; a DDC asking for less is
// decimated here. DDCs the gateware or the bus cannot run are sent as
// zeros. TX I/Q arrives at 192 ksps and is decimated to the gateware's
// 48 ksps. MOX, drive and PA enable go through the TxPolicy as in
// Protocol 1. The radio's own sample clock paces the silent Mic/Line
// packets that clock the client's transmitter, and the High Priority
// status (every 50 ms, and at once when the PTT or key input changes).
class Server {
public:
    Server(Identity identity, Control& control, SpscRing<TxBlock>& tx, Session& session,
           TxPolicy& policy, std::function<p1::GatewareStatus()> status, Link& link);
    ~Server();

    // `port` is the offset from the base port the packet arrived on.
    void on_packet(unsigned port, std::span<const std::uint8_t> packet, const Peer& from,
                   std::chrono::nanoseconds now);
    void on_rx(const RxBlock& block);
    // Stops streaming to a client that no longer owns the radio (call
    // after Session::tick).
    void tick();

    void set_frequency_ppm(double ppm);
    void set_iq_convention(p1::IqConvention convention);
    // The LNA gain (-12 to 48 dB) with no step attenuation; the client's
    // ADC0 step attenuation is taken off it.
    void set_rx_gain_db(int db);

    bool running() const;
    std::optional<Peer> client() const;
    unsigned device_receivers() const;
    unsigned device_rate_code() const;
    ServerCounters counters() const;

private:
    struct DdcStream;
    struct Outgoing {
        unsigned port = 0;
        std::size_t size = 0;
        LongPacket bytes{};
    };

    static Owner owner(const Peer& peer) { return {Protocol::P2, peer}; }
    bool claim_locked(const Peer& from, std::chrono::nanoseconds now);
    void start_locked();
    void general_locked(const General& g);
    void rx_specific_locked(const RxSpecific& r);
    void tx_specific_locked(const TxSpecific& t);
    void high_priority_locked(const HighPriority& h, std::chrono::nanoseconds now);
    void tx_iq_locked(const TxIq& t);
    void command_locked(std::uint8_t address, std::uint32_t data);
    void drive_locked();
    void gain_locked();
    void check_sequence_locked(std::optional<std::uint32_t>& last, std::uint32_t sequence);
    Outgoing& outgoing_locked(std::size_t& n);

    const Identity identity_;
    Control& control_;
    SpscRing<TxBlock>& tx_;
    Session& session_;
    TxPolicy& policy_;
    std::function<p1::GatewareStatus()> status_;
    Link& link_;

    mutable std::mutex mutex_;
    bool running_ = false;
    Peer client_;
    bool phase_words_ = false;
    bool pa_enable_ = false;
    std::uint8_t drive_ = 0;
    std::uint8_t attenuation_ = 0;
    int rx_gain_db_ = 14;
    double frequency_ppm_ = 0;
    bool swap_iq_ = false;
    unsigned device_receivers_ = 1;
    unsigned device_rate_code_ = 0;
    std::vector<std::unique_ptr<DdcStream>> ddcs_;
    std::optional<std::uint32_t> last_general_;
    std::optional<std::uint32_t> last_rx_specific_;
    std::optional<std::uint32_t> last_tx_specific_;
    std::optional<std::uint32_t> last_high_priority_;
    std::optional<std::uint32_t> last_tx_iq_;
    dsp::Decimator tx_decimator_;
    std::uint32_t mic_sequence_ = 0;
    std::uint64_t mic_accumulator_ = 0;  // 48 kHz samples owed, times the device rate
    std::uint32_t status_sequence_ = 0;
    std::uint64_t status_frames_ = 0;  // device frames since the last status
    std::optional<std::pair<bool, bool>> status_sent_;
    // What has been reported once.
    bool warned_ports_ = false;
    bool warned_wideband_ = false;
    bool warned_sync_ = false;
    bool warned_adc_ = false;
    bool warned_tx_format_ = false;
    bool warned_cw_ = false;
    ServerCounters counters_;

    // Used by on_rx only.
    std::vector<Outgoing> outgoing_;
};

}  // namespace radioberry::daemon::p2
