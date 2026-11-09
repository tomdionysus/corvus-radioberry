// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

#include "device/service.hpp"
#include "net.hpp"
#include "p1/server.hpp"
#include "p2/network.hpp"
#include "p2/server.hpp"
#include "radioberry/device.hpp"
#include "session.hpp"
#include "tx_policy.hpp"

namespace radioberry::daemon {

struct DaemonOptions {
    std::uint32_t address = 0;  // host byte order; 0 is every interface
    std::uint16_t port = p1::kPort;
    std::optional<std::array<std::uint8_t, 6>> mac;  // default: interface_mac()
    // A streaming client silent this long loses the radio.
    std::chrono::nanoseconds client_timeout = std::chrono::seconds(3);
    // The radio's frequency error in ppm (positive: it tunes high).
    double frequency_ppm = 0;
    p1::IqConvention iq_convention = p1::IqConvention::Hl2;
    // Protocol 2: the LNA gain with no step attenuation, dB (-12 to 48).
    int p2_rx_gain_db = 14;
    // SCHED_FIFO for the stream and network threads and locked memory
    // (realtime.hpp); without the privileges each is reported and skipped.
    bool realtime = true;
    // The driver's TX ring is kept within 100 ms at 48 ksps.
    DeviceOptions device{.tx_ring_limit_words = 4800, .on_thread_start = {}};
    TxPolicyOptions tx;
};

// radioberryd: the device layer, the session, the Protocol 1 and Protocol 2
// servers and their sockets. Protocol 2 uses the Protocol 1 port as its
// base port, shared, and the ports after it.
// The calling thread runs the Protocol 1 network, a second the Protocol 2
// network, and a third hands RX blocks to both servers; the one with a
// running client streams them.
class Daemon {
public:
    // Reads the gateware identity and binds the sockets; throws
    // std::system_error when either fails.
    Daemon(Device& device, const DaemonOptions& options);
    ~Daemon();
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    // Runs until `stop` is set or the device fails, then leaves the
    // gateware in the safe state. False if the device failed.
    bool run(const std::atomic<bool>& stop);

    std::uint16_t port() const { return network_.port(); }
    // The Protocol 2 port at `offset` from the base (p2/protocol2.hpp).
    std::uint16_t p2_port(unsigned offset) const {
        return offset == p2::kGeneralPort ? network_.port() : p2_network_.port(offset);
    }
    const p1::Identity& identity() const { return identity_; }
    p1::Server& server() { return server_; }
    p2::Server& p2_server() { return p2_server_; }
    Session& session() { return session_; }
    TxPolicy& policy() { return policy_; }
    DeviceService& device() { return service_; }

private:
    void on_fault(const Fault& fault);
    // Names the calling thread (unless it is the main thread) and, when
    // real-time is on, sets its priority; the first failure is logged.
    void start_thread(const char* name, int priority, bool rename = true);
    void on_packet(std::span<const std::uint8_t> packet, const Peer& from,
                   std::chrono::nanoseconds now);

    p1::Identity identity_;
    Session session_;
    DeviceService service_;
    TxPolicy policy_;
    Network network_;
    p1::Server server_;
    p2::Network p2_network_;
    p2::Server p2_server_;
    double frequency_ppm_ = 0;
    bool iq_standard_ = false;
    bool realtime_ = true;
    std::atomic<bool> realtime_failed_{false};
};

}  // namespace radioberry::daemon
