// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <radioberry_ioctl.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace radioberry::daemon {

enum class FaultKind {
    RxResync,        // the driver lost RX frame alignment
    RxDroppedWords,  // RX words discarded while resynchronising
    RxOverrun,       // RX frames dropped: the driver's ring was full
    RxQueueFull,     // RX blocks dropped: the protocol side fell behind
    RxStall,         // no RX frames for the stall time while running
    TxUnderrun,      // the driver's TX ring ran empty with PTT set
    TxInvalidBlock,  // a TX block not of whole samples, dropped
    DeviceError,     // a call on the device failed
};

// One fault: `count` new events of `kind` since the last report.
struct Fault {
    FaultKind kind;
    std::uint64_t count = 1;
    std::string detail;
};

std::string_view fault_name(FaultKind kind);
// "rx-resync: 3", "device-error: /dev/radioberry: read: ...".
std::string describe(const Fault& fault);

// Receives faults from the device threads; must be thread-safe.
using FaultSink = std::function<void(const Fault&)>;
// Monotonic time; steady_clock in the daemon, a fake in tests.
using Clock = std::function<std::chrono::nanoseconds()>;
Clock steady_clock();

// Turns successive driver statistics into faults: every counter that
// moved, and an RX stall when no frame arrives for `stall_after` while
// frames are expected. A stall is reported once, until frames resume.
class Monitor {
public:
    Monitor(FaultSink sink, std::chrono::nanoseconds stall_after);

    // A snapshot taken at `now`. `rx_expected` is false while nothing
    // should stream (run clear), which restarts the stall timer. The
    // first snapshot sets the baseline and reports nothing.
    void update(const radioberry_stats& stats, std::chrono::nanoseconds now, bool rx_expected);

    bool stalled() const;
    std::optional<radioberry_stats> last() const;

private:
    void report(FaultKind kind, std::uint64_t before, std::uint64_t after);

    FaultSink sink_;
    std::chrono::nanoseconds stall_after_;
    mutable std::mutex mutex_;
    std::optional<radioberry_stats> last_;
    std::chrono::nanoseconds last_frame_at_{};
    bool rx_expected_ = false;  // at the last snapshot
    bool stalled_ = false;
};

}  // namespace radioberry::daemon
