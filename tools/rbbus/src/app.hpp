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

namespace radioberry::bus {

enum ExitCode : int {
    kExitOk = 0,
    kExitFailed = 1,
    kExitUsage = 2,
};

enum class ReportFormat { kText, kJson, kYaml };

struct Options {
    double seconds = 10;
    unsigned receivers = 1;
    unsigned rate_code = 0;  // 48 ksps
    unsigned settle_reads = 0;
    // Microseconds to sleep when the gateware has nothing to send; 0 spins.
    unsigned idle_sleep_us = 1000;
    // Also clock TX words (zeros, at 48 000 per second) with PTT and the TX
    // permission bit clear.
    bool tx = false;
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

// What rb_bitbang.h counts while clocking.
struct BusCounters {
    std::uint64_t rx_clocks = 0;
    std::uint64_t rx_idle_clocks = 0;
    std::uint64_t rx_words = 0;
    std::uint64_t tx_clocks = 0;
    std::uint64_t tx_ready_waits = 0;
    std::uint64_t tx_words = 0;
};

// The RX and TX buses clocked by the CPU (driver/kernel/rb_bitbang.h).
class Bus {
public:
    virtual ~Bus() = default;
    // rb_bb_rx_run(): words are index << 24 | value.
    virtual unsigned rx_run(std::uint32_t* out, unsigned max_words, unsigned max_idle) = 0;
    // rb_bb_tx_run().
    virtual unsigned tx_run(const std::uint32_t* words, unsigned count, unsigned max_polls) = 0;
    virtual void set_settle_reads(unsigned reads) = 0;
    virtual BusCounters counters() const = 0;
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
    std::function<std::unique_ptr<Bus>()> open_bus;
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

// The command that clears run, PTT, the TX permission bit, PA enable and
// drive (the driver's rb_cmd_safe()).
Command safe_command();

}  // namespace radioberry::bus
