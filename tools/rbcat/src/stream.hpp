// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <vector>

#include "options.hpp"
#include "radioberry/device.hpp"

namespace radioberry::cat {

// Monotonic seconds; a fake in the tests.
class Clock {
public:
    virtual ~Clock() = default;
    virtual double now() = 0;
};

// Signal level of one receiver over the measurement.
struct Level {
    double power_sum = 0;  // sum of I^2 + Q^2
    std::uint64_t samples = 0;
    std::int32_t peak = 0;  // largest |I| or |Q|

    // Mean power relative to a full-scale complex sinusoid; nothing when
    // there were no samples or all were zero.
    std::optional<double> rms_dbfs() const;
    std::optional<double> peak_dbfs() const;
};

struct StreamResult {
    radioberry_info info{};  // after configuration
    std::vector<Command> commands;
    radioberry_stats before{};         // before configuration
    radioberry_stats measure_start{};  // after settling
    radioberry_stats end{};
    double seconds = 0;  // measured
    std::uint64_t frames = 0;
    std::uint64_t bytes_out = 0;
    std::vector<Level> levels;
    bool interrupted = false;
};

// Configures the receivers and streams until `options.seconds` have been
// measured or `stop` returns true. Frames go to `samples` in the chosen
// output format (ignored for Output::kNone); progress lines to `progress`.
// Refuses any command that could transmit. Throws std::runtime_error when
// the device cannot do what was asked, std::system_error on device errors.
StreamResult run_stream(Device& device, Clock& clock, const Options& options, std::ostream& samples,
                        std::ostream& progress, const std::function<bool()>& stop);

// Sends a command that cannot transmit; throws std::logic_error for one
// that could.
Reply send_receive_only(Device& device, const Command& command);

}  // namespace radioberry::cat
