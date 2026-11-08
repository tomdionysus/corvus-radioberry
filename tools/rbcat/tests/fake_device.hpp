// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "radioberry/device.hpp"
#include "stream.hpp"

namespace radioberry::cat::test {

class FakeClock final : public Clock {
public:
    double now() override { return t; }
    double t = 1000.0;
};

// A CL025 behind radioberry.ko. Each read delivers `read_seconds` of
// frames at the configured rate (times `rate_scale`) and advances the
// clock by that much. Every receiver carries the constant sample
// (`i_value`, `q_value`). The TX ring drains at 48 ksps of fake time,
// `tx_rate_scale` times that.
class FakeDevice final : public Device {
public:
    explicit FakeDevice(FakeClock& clock);

    radioberry_info info() override;
    radioberry_stats stats() override;
    Reply command(const Command& command) override;
    std::size_t read(std::span<std::uint8_t> buffer) override;
    std::size_t write(std::span<const std::uint8_t> buffer) override;

    FakeClock& clock;
    radioberry_info info_{};
    radioberry_stats stats_{};
    std::vector<Command> commands;
    bool follow_commands = true;
    std::int32_t i_value = 1 << 22;
    std::int32_t q_value = 0;
    double read_seconds = 0.001;
    double rate_scale = 1.0;
    // Reads that return 0, as when interrupted by a signal.
    int interrupted_reads = 0;
    // Called after every read, for fault injection.
    std::function<void(FakeDevice&)> on_read;
    std::size_t reads = 0;
    std::size_t stats_calls = 0;

    std::uint32_t tx_capacity_words = 16384;
    double tx_rate_scale = 1.0;
    double tx_drain_credit = 0;
    // Most bytes one write accepts; writes that return 0.
    std::size_t tx_write_limit = SIZE_MAX;
    int interrupted_writes = 0;
    std::uint64_t tx_bytes_written = 0;
    bool tx_nonzero_written = false;
};

}  // namespace radioberry::cat::test
