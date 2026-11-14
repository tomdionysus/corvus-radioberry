// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_device.hpp"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <system_error>
#include <thread>

namespace radioberry::daemon::test {

FakeDevice::FakeDevice() {
    info_.abi_version = RADIOBERRY_ABI_VERSION;
    info_.driver_version = 0x000400;
    info_.gateware_major = 75;
    info_.gateware_minor = 3;
    info_.fpga_type = RADIOBERRY_FPGA_CL025;
    info_.receivers = 6;
    info_.transmitters = 1;
    info_.active_receivers = 1;
    info_.rx_rate_code = 0;
    reply.transmitters = 1;
    reply.receivers = 6;
    reply.fpga_type = RADIOBERRY_FPGA_CL025;
    reply.version_major = 75;
    reply.version_minor = 3;
}

void FakeDevice::maybe_fail(const char* op) {
    if (fail_on == op) {
        fail_on.clear();
        throw std::system_error(EIO, std::generic_category(), std::string("fake: ") + op);
    }
}

radioberry_info FakeDevice::info() {
    const std::lock_guard lock(mutex);
    maybe_fail("info");
    return info_;
}

radioberry_stats FakeDevice::stats() {
    const std::lock_guard lock(mutex);
    maybe_fail("stats");
    ++stats_calls;
    radioberry_stats s = stats_;
    s.flags = overrun ? RADIOBERRY_STATS_RX_OVERRUN : 0u;
    overrun = false;
    return s;
}

Reply FakeDevice::command(const Command& command) {
    const std::lock_guard lock(mutex);
    maybe_fail("command");
    commands.push_back(command);
    ++stats_.commands;
    if (command.address == 0x00) {
        const auto receivers =
            std::min<unsigned>(((command.data >> 3) & 0x0fu) + 1, info_.receivers);
        if (receivers != info_.active_receivers) {
            frames_ready = 0;  // the driver resets its RX ring
        }
        info_.active_receivers = static_cast<std::uint8_t>(receivers);
        info_.rx_rate_code = static_cast<std::uint8_t>(command.data >> 24 & 0x03u);
    }
    return reply;
}

std::size_t FakeDevice::read(std::span<std::uint8_t> buffer) {
    const std::lock_guard lock(mutex);
    maybe_fail("read");
    const std::size_t frame = rx_frame_bytes(info_.active_receivers);
    const std::size_t n = std::min(frames_ready, buffer.size() / frame);
    for (std::size_t f = 0; f < n; ++f, ++frame_index) {
        std::fill_n(buffer.begin() + static_cast<std::ptrdiff_t>(f * frame), frame,
                    static_cast<std::uint8_t>(frame_index));
    }
    frames_ready -= n;
    stats_.rx_frames += n;
    return n * frame;
}

std::size_t FakeDevice::write(std::span<const std::uint8_t> buffer) {
    const std::lock_guard lock(mutex);
    maybe_fail("write");
    const std::size_t room = std::size_t{tx_capacity_words - stats_.tx_ring_words} * kTxSampleBytes;
    const std::size_t n =
        std::min({buffer.size(), room, write_limit}) / kTxSampleBytes * kTxSampleBytes;
    written.insert(written.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
    stats_.tx_ring_words += static_cast<std::uint32_t>(n / kTxSampleBytes);
    return n;
}

Readiness FakeDevice::wait(bool want_read, bool want_write, int timeout_ms) {
    Readiness r;
    {
        const std::lock_guard lock(mutex);
        maybe_fail("wait");
        ++waits;
        r.readable = want_read && (frames_ready > 0 || always_readable);
        r.writable = want_write && stats_.tx_ring_words < tx_capacity_words;
        r.error = overrun;
    }
    if (!r.readable && !r.writable && !r.error && idle_wait_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(idle_wait_ms, timeout_ms)));
    }
    return r;
}

Readiness FakeDevice::wait_or_wake(bool want_read, bool want_write, int timeout_ms, int wake_fd) {
    Readiness r = wait(want_read, want_write, 0);
    {
        const std::lock_guard lock(mutex);
        last_timeout_ms = timeout_ms;
    }
    if (r.readable || r.writable || r.error) {
        return r;
    }
    int ms = 0;
    {
        const std::lock_guard lock(mutex);
        ms = timeout_ms < 0 ? std::max(idle_wait_ms, 1) : std::min(idle_wait_ms, timeout_ms);
    }
    if (wake_fd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return r;
    }
    pollfd p{wake_fd, POLLIN, 0};
    r.woken = ::poll(&p, 1, ms) > 0;
    return r;
}

void FakeDevice::drain_tx(std::uint32_t words) {
    const std::lock_guard lock(mutex);
    const std::uint32_t n = std::min(words, stats_.tx_ring_words);
    stats_.tx_ring_words -= n;
    stats_.tx_words += n;
}

}  // namespace radioberry::daemon::test
