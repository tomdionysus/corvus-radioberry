// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/monitor.hpp"

#include <utility>

namespace radioberry::daemon {

std::string_view fault_name(FaultKind kind) {
    switch (kind) {
        case FaultKind::RxResync:
            return "rx-resync";
        case FaultKind::RxDroppedWords:
            return "rx-dropped-words";
        case FaultKind::RxOverrun:
            return "rx-overrun";
        case FaultKind::RxQueueFull:
            return "rx-queue-full";
        case FaultKind::RxStall:
            return "rx-stall";
        case FaultKind::TxUnderrun:
            return "tx-underrun";
        case FaultKind::TxInvalidBlock:
            return "tx-invalid-block";
        case FaultKind::DeviceError:
            return "device-error";
    }
    return "unknown";
}

std::string describe(const Fault& fault) {
    std::string text(fault_name(fault.kind));
    text += ": ";
    text += fault.detail.empty() ? std::to_string(fault.count) : fault.detail;
    return text;
}

Clock steady_clock() {
    return [] { return std::chrono::steady_clock::now().time_since_epoch(); };
}

Monitor::Monitor(FaultSink sink, std::chrono::nanoseconds stall_after)
    : sink_(std::move(sink)), stall_after_(stall_after) {}

void Monitor::update(const radioberry_stats& s, std::chrono::nanoseconds now, bool rx_expected) {
    bool stall_now = false;
    {
        const std::lock_guard lock(mutex_);
        if (!last_) {
            last_ = s;
            last_frame_at_ = now;
            rx_expected_ = rx_expected;
            return;
        }
        const radioberry_stats before = *last_;
        last_ = s;
        report(FaultKind::RxResync, before.rx_resyncs, s.rx_resyncs);
        report(FaultKind::RxDroppedWords, before.rx_dropped_words, s.rx_dropped_words);
        report(FaultKind::RxOverrun, before.rx_overrun_frames, s.rx_overrun_frames);
        report(FaultKind::TxUnderrun, before.tx_underruns, s.tx_underruns);
        // While idle the daemon takes no snapshots, so the timer restarts
        // when frames become expected, not at the last idle snapshot.
        if (s.rx_frames != before.rx_frames || !rx_expected || !rx_expected_) {
            last_frame_at_ = now;
            stalled_ = false;
        } else if (!stalled_ && now - last_frame_at_ >= stall_after_) {
            stalled_ = true;
            stall_now = true;
        }
        rx_expected_ = rx_expected;
    }
    if (stall_now) {
        sink_({FaultKind::RxStall, 1, {}});
    }
}

bool Monitor::stalled() const {
    const std::lock_guard lock(mutex_);
    return stalled_;
}

std::optional<radioberry_stats> Monitor::last() const {
    const std::lock_guard lock(mutex_);
    return last_;
}

void Monitor::report(FaultKind kind, std::uint64_t before, std::uint64_t after) {
    if (after > before) {
        sink_({kind, after - before, {}});
    }
}

}  // namespace radioberry::daemon
