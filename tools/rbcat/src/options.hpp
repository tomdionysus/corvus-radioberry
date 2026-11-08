// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace radioberry::cat {

// What goes to stdout: RX frames as the driver delivers them (24-bit
// big-endian I and Q per receiver), the top 16 bits of each value as
// little-endian int16, or nothing.
enum class Output { kNone, kIq24, kIq16 };
enum class ReportFormat { kText, kJson, kYaml };

inline constexpr std::uint32_t kMaxFrequencyHz = 38'400'000;

struct Options {
    std::string device = "/dev/radioberry";
    unsigned rate_ksps = 48;
    unsigned rate_code = 0;
    unsigned receivers = 1;
    // One per receiver after parsing.
    std::vector<std::uint32_t> frequencies_hz = {14'074'000};
    int lna_gain_db = 20;
    Output output = Output::kNone;
    // 0 runs until interrupted.
    double seconds = 0;
    // Time after configuration before measuring starts.
    double settle_seconds = 0.5;
    // Progress line on stderr every this many seconds; 0 for none.
    double interval_seconds = 0;
    // Also write zero TX samples at the gateware's 48 ksps, PTT and the
    // TX permission bit clear.
    bool tx_zeros = false;
    ReportFormat format = ReportFormat::kText;
};

struct ShowHelp {};
struct ShowVersion {};
struct UsageError {
    std::string message;
};

using ParseResult = std::variant<Options, ShowHelp, ShowVersion, UsageError>;

ParseResult parse_options(std::span<const std::string> args);

extern const char* const kUsage;

std::string to_string(Output output);

}  // namespace radioberry::cat
