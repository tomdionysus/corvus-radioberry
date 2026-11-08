// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_device.hpp"

#include <algorithm>
#include <cmath>

namespace radioberry::cat::test {
namespace {

void put24(std::uint8_t* p, std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    p[0] = static_cast<std::uint8_t>(u >> 16);
    p[1] = static_cast<std::uint8_t>(u >> 8);
    p[2] = static_cast<std::uint8_t>(u);
}

}  // namespace

FakeDevice::FakeDevice(FakeClock& c) : clock(c) {
    info_.abi_version = RADIOBERRY_ABI_VERSION;
    info_.driver_version = 0x000102;
    info_.gateware_major = 75;
    info_.gateware_minor = 3;
    info_.fpga_type = RADIOBERRY_FPGA_CL025;
    info_.receivers = 6;
    info_.transmitters = 1;
    info_.active_receivers = 1;
    info_.rx_rate_code = 0;
    stats_.rx_period_bytes = 288;
}

radioberry_info FakeDevice::info() { return info_; }

radioberry_stats FakeDevice::stats() {
    ++stats_calls;
    return stats_;
}

Reply FakeDevice::command(const Command& command) {
    commands.push_back(command);
    stats_.commands += 1;
    if (follow_commands && command.address == 0x00) {
        info_.active_receivers = static_cast<std::uint8_t>(((command.data >> 3) & 0x0fu) + 1);
        info_.rx_rate_code = static_cast<std::uint8_t>((command.data >> 24) & 0x03u);
    }
    Reply r;
    r.transmitters = info_.transmitters;
    r.receivers = info_.receivers;
    r.fpga_type = info_.fpga_type;
    r.version_major = info_.gateware_major;
    r.version_minor = info_.gateware_minor;
    return r;
}

std::size_t FakeDevice::read(std::span<std::uint8_t> buffer) {
    ++reads;
    if (interrupted_reads > 0) {
        --interrupted_reads;
        return 0;
    }
    const unsigned n = info_.active_receivers;
    const std::size_t frame = rx_frame_bytes(n);
    const double rate = (48000u << info_.rx_rate_code) * rate_scale;
    const auto wanted = static_cast<std::size_t>(std::llround(rate * read_seconds));
    const std::size_t frames = std::min(wanted, buffer.size() / frame);
    for (std::size_t f = 0; f < frames; ++f) {
        for (unsigned r = 0; r < n; ++r) {
            put24(&buffer[f * frame + 6 * r], i_value);
            put24(&buffer[f * frame + 6 * r + 3], q_value);
        }
    }
    const double dt = static_cast<double>(frames) / rate;
    clock.t += dt;
    tx_drain_credit += 48000.0 * tx_rate_scale * dt;
    const auto drained =
        std::min<std::uint32_t>(stats_.tx_ring_words, static_cast<std::uint32_t>(tx_drain_credit));
    tx_drain_credit -= drained;
    if (stats_.tx_ring_words == 0) {
        tx_drain_credit = 0;
    }
    stats_.tx_ring_words -= drained;
    stats_.tx_words += drained;
    stats_.rx_frames += frames;
    stats_.rx_words += frames * 2 * n;
    if (on_read) {
        on_read(*this);
    }
    return frames * frame;
}

std::size_t FakeDevice::write(std::span<const std::uint8_t> buffer) {
    if (interrupted_writes > 0) {
        --interrupted_writes;
        return 0;
    }
    const std::size_t free_bytes =
        static_cast<std::size_t>(tx_capacity_words - stats_.tx_ring_words) * kTxSampleBytes;
    const std::size_t n = std::min({buffer.size(), free_bytes, tx_write_limit}) / 4 * 4;
    tx_nonzero_written =
        tx_nonzero_written || std::any_of(buffer.begin(), buffer.begin() + static_cast<long>(n),
                                          [](std::uint8_t b) { return b != 0; });
    stats_.tx_ring_words += static_cast<std::uint32_t>(n / kTxSampleBytes);
    tx_bytes_written += n;
    return n;
}

}  // namespace radioberry::cat::test
