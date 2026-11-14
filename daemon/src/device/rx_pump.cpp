// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/rx_pump.hpp"

#include <exception>
#include <string>
#include <utility>

namespace radioberry::daemon {

RxPump::RxPump(Device& device, Control& control, SpscRing<RxBlock>& ring, Monitor& monitor,
               FaultSink sink, Clock clock, Options options)
    : device_(device),
      control_(control),
      ring_(ring),
      monitor_(monitor),
      sink_(std::move(sink)),
      clock_(std::move(clock)),
      options_(options) {
    control_.set_notify([this] { wake_.signal(); });
}

RxPump::~RxPump() { control_.set_notify({}); }

bool RxPump::start() {
    try {
        const radioberry_info info = device_.info();
        receivers_ = info.active_receivers;
        rate_code_ = info.rx_rate_code;
        poll_stats();
        return true;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

bool RxPump::step() {
    if (failed_) {
        return false;
    }
    try {
        while (const auto command = control_.next()) {
            send(*command);
        }
        const bool run = control_.run();
        const Readiness ready =
            device_.wait_or_wake(true, false, run ? options_.wait_ms : -1, wake_.fd());
        if (ready.woken) {
            wake_.drain();
        }
        if (ready.error || (run && clock_() - last_stats_ >= options_.stats_period)) {
            poll_stats();
        }
        if (ready.readable) {
            read();
        }
        return true;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

bool RxPump::flush_commands() {
    if (failed_) {
        return false;
    }
    try {
        while (const auto command = control_.next()) {
            send(*command);
        }
        return true;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

void RxPump::send(const Command& command) {
    const Reply reply = device_.command(command);
    ++commands_;
    cw_or_ptt_ = reply.cw_or_ptt;
    external_cw_key_ = reply.external_cw_key;
    if (command.address == Control::kGeneral) {
        // The driver clamps the count to the gateware's receivers.
        const radioberry_info info = device_.info();
        receivers_ = info.active_receivers;
        rate_code_ = info.rx_rate_code;
    }
}

void RxPump::poll_stats() {
    const auto now = clock_();
    monitor_.update(device_.stats(), now, control_.run());
    last_stats_ = now;
}

void RxPump::read() {
    const unsigned receivers = receivers_;
    const std::size_t frame = rx_frame_bytes(receivers);
    RxBlock* block = ring_.acquire();
    RxBlock& target = block != nullptr ? *block : scratch_;
    const std::size_t n = device_.read(
        std::span<std::uint8_t>(target.bytes.data(), RxBlock::kCapacity / frame * frame));
    if (n == 0) {
        return;
    }
    ++sequence_;
    if (block == nullptr) {
        ++blocks_dropped_;
        if (!dropping_) {
            dropping_ = true;
            sink_({FaultKind::RxQueueFull, 1, {}});
        }
        return;
    }
    target.size = n;
    target.receivers = receivers;
    target.rate_code = rate_code_;
    target.sequence = sequence_;
    ring_.publish();
    ++blocks_;
    dropping_ = false;
}

bool RxPump::fail(const char* what) {
    failed_ = true;
    sink_({FaultKind::DeviceError, 1, what});
    return false;
}

}  // namespace radioberry::daemon
