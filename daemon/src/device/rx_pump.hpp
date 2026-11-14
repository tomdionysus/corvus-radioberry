// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "device/control.hpp"
#include "device/monitor.hpp"
#include "device/ring.hpp"
#include "event.hpp"
#include "radioberry/device.hpp"

namespace radioberry::daemon {

// Whole RX frames from one read, in the driver's layout: per frame, I then
// Q for each active receiver, 24-bit big-endian (radioberry_ioctl.h).
struct RxBlock {
    static constexpr std::size_t kCapacity = 16384;
    std::array<std::uint8_t, kCapacity> bytes{};
    std::size_t size = 0;
    unsigned receivers = 1;
    unsigned rate_code = 0;
    // Counts every block read, so a gap shows blocks dropped.
    std::uint64_t sequence = 0;

    std::size_t frames() const { return size / rx_frame_bytes(receivers); }
};

// The RX device thread's work. It sends the queued commands as well as
// reading, so it always knows the frame layout of what it reads: a
// receiver-count command takes effect between two of its reads.
class RxPump {
public:
    struct Options {
        std::chrono::nanoseconds stats_period = std::chrono::milliseconds(100);
        int wait_ms = 10;  // longest wait for frames while run is set
    };

    RxPump(Device& device, Control& control, SpscRing<RxBlock>& ring, Monitor& monitor,
           FaultSink sink, Clock clock, Options options);
    ~RxPump();
    RxPump(const RxPump&) = delete;
    RxPump& operator=(const RxPump&) = delete;

    // Reads the current layout from the driver. False on a device error.
    bool start();
    // One pass: sends every queued command, then waits for frames, a new
    // command (Control notifies) or wake(). With run set it waits up to
    // wait_ms and polls the statistics when due; with run clear nothing
    // streams and it waits without limit, so an idle radio costs no
    // wake-ups. Statistics are also polled when an RX overrun or a device
    // error is pending. Reads into one block; when the ring is full the
    // block is read and dropped, and an rx-queue-full fault is reported
    // once until a block is delivered again. Returns false after a device
    // error, which is reported as a fault and ends the pump.
    bool step();
    // Ends the current wait at once (for stopping). Thread-safe.
    void wake() const noexcept { wake_.signal(); }
    // Sends every queued command. False on a device error.
    bool flush_commands();

    bool failed() const { return failed_.load(); }
    unsigned receivers() const { return receivers_.load(); }
    unsigned rate_code() const { return rate_code_.load(); }
    std::uint64_t commands() const { return commands_.load(); }
    std::uint64_t blocks() const { return blocks_.load(); }
    std::uint64_t blocks_dropped() const { return blocks_dropped_.load(); }
    // From the gateware's latest reply: cw_on or external PTT, and the
    // external CW key.
    bool cw_or_ptt() const { return cw_or_ptt_.load(); }
    bool external_cw_key() const { return external_cw_key_.load(); }

private:
    void send(const Command& command);
    void poll_stats();
    void read();
    bool fail(const char* what);

    Device& device_;
    Control& control_;
    SpscRing<RxBlock>& ring_;
    Monitor& monitor_;
    FaultSink sink_;
    Clock clock_;
    Options options_;
    Event wake_;
    RxBlock scratch_;
    std::chrono::nanoseconds last_stats_{};
    std::uint64_t sequence_ = 0;
    bool dropping_ = false;

    std::atomic<bool> failed_{false};
    std::atomic<unsigned> receivers_{1};
    std::atomic<unsigned> rate_code_{0};
    std::atomic<std::uint64_t> commands_{0};
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> blocks_dropped_{0};
    std::atomic<bool> cw_or_ptt_{false};
    std::atomic<bool> external_cw_key_{false};
};

}  // namespace radioberry::daemon
