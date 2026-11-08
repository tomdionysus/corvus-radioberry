// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

#include "device/control.hpp"
#include "device/monitor.hpp"
#include "device/ring.hpp"
#include "device/rx_pump.hpp"
#include "device/tx_pump.hpp"
#include "radioberry/device.hpp"

namespace radioberry::daemon {

struct DeviceOptions {
    std::size_t rx_blocks = 64;
    std::size_t tx_blocks = 64;
    std::chrono::nanoseconds stats_period = std::chrono::milliseconds(100);
    std::chrono::nanoseconds stall_after = std::chrono::milliseconds(250);
    int wait_ms = 10;
    // See TxPump; 0 is no limit.
    std::uint32_t tx_ring_limit_words = 0;
};

// What the device layer has done and seen, for status reports and tests.
struct DeviceSnapshot {
    std::optional<radioberry_stats> driver;  // latest driver statistics
    unsigned receivers = 0;
    unsigned rate_code = 0;
    std::uint64_t commands = 0;
    std::uint64_t rx_blocks = 0;
    std::uint64_t rx_blocks_dropped = 0;
    std::uint64_t tx_blocks = 0;
    std::uint64_t tx_bytes = 0;
    std::uint64_t tx_blocks_dropped = 0;
    bool cw_or_ptt = false;
    bool external_cw_key = false;
    bool stalled = false;
    bool failed = false;
};

// The daemon's device layer over /dev/radioberry (opened non-blocking):
// the command registers, an RX thread that sends commands and reads
// frames into the RX ring, and a TX thread that writes the TX ring to the
// driver. Faults from either thread go to the sink. Protocol code consumes
// rx() and produces tx(), one thread each.
class DeviceService {
public:
    DeviceService(Device& device, FaultSink sink, DeviceOptions options = {},
                  Clock clock = steady_clock());
    ~DeviceService();
    DeviceService(const DeviceService&) = delete;
    DeviceService& operator=(const DeviceService&) = delete;

    // Reads the layout and starts both threads. False, with a fault
    // reported, when the device fails.
    bool start();
    // Puts the gateware in the safe state (Control::safe), sends it and
    // every other queued command, and stops both threads. The driver
    // sends its own safe command when the device is closed.
    void stop();
    bool running() const { return running_; }

    Control& control() { return control_; }
    SpscRing<RxBlock>& rx() { return rx_ring_; }
    SpscRing<TxBlock>& tx() { return tx_ring_; }
    DeviceSnapshot snapshot() const;
    // A device call failed in either thread.
    bool failed() const { return rx_pump_.failed() || tx_pump_.failed(); }

private:
    Control control_;
    SpscRing<RxBlock> rx_ring_;
    SpscRing<TxBlock> tx_ring_;
    Monitor monitor_;
    RxPump rx_pump_;
    TxPump tx_pump_;
    std::atomic<bool> stop_rx_{false};
    std::atomic<bool> stop_tx_{false};
    std::thread rx_thread_;
    std::thread tx_thread_;
    bool running_ = false;
};

}  // namespace radioberry::daemon
