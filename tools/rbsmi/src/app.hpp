// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <variant>

#include "radioberry/command.hpp"

namespace radioberry::smi {

enum ExitCode : int {
    kExitOk = 0,
    kExitFailed = 1,
    kExitUsage = 2,
};

enum class ReportFormat { kText, kJson, kYaml };

// SMI read timing, in cycles of the SMI clock (125 MHz on a Pi 4: 8 ns).
// One read strobe takes setup + strobe + hold + pace cycles.
struct Timing {
    unsigned setup = 2;    // 1 to 63
    unsigned strobe = 10;  // 1 to 127
    unsigned hold = 2;     // 0 to 63
    unsigned pace = 16;    // 0 to 127
    unsigned cycles() const { return setup + strobe + hold + pace; }
};

struct Options {
    double seconds = 10;
    unsigned receivers = 1;
    unsigned rate_code = 0;  // 48 ksps
    Timing timing;
    unsigned read_words = 16384;  // words per SMI transfer
    std::string spi = "/dev/spidev0.0";
    ReportFormat format = ReportFormat::kText;
};

struct ShowHelp {};
struct ShowVersion {};
struct UsageError {
    std::string message;
};

using ParseResult = std::variant<Options, ShowHelp, ShowVersion, UsageError>;

ParseResult parse_options(std::span<const std::string> args);

// 18-bit SMI reads. The strobes run only while a transfer is active, so
// the RX clock stops between reads.
class Reader {
public:
    virtual ~Reader() = default;
    // Sets 18-bit reads with this timing.
    virtual void configure(const Timing& timing) = 0;
    // One transfer: fills `words` with one word per read strobe; returns
    // how many.
    virtual std::size_t read(std::span<std::uint32_t> words) = 0;
};

// Gateware commands over SPI.
class Control {
public:
    virtual ~Control() = default;
    virtual std::array<std::uint8_t, 6> transfer(const std::array<std::uint8_t, 6>& tx) = 0;
};

struct Environment {
    // /proc/device-tree/compatible, empty if unreadable.
    std::function<std::string()> compatible;
    // True when /dev/radioberry exists: the driver owns the bus pins.
    std::function<bool()> driver_bound;
    std::function<std::unique_ptr<Reader>()> open_reader;
    std::function<std::unique_ptr<Control>(const std::string& device)> open_control;
    std::function<std::chrono::steady_clock::duration()> now;
    std::function<void(std::chrono::microseconds)> sleep;
    // CPU time this process has used, user and system, in seconds.
    std::function<double()> cpu_seconds;
    std::ostream& out;
    std::ostream& err;
};

int run_app(std::span<const std::string> args, Environment& env);

extern const char* const kUsage;

}  // namespace radioberry::smi
