// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <variant>

#include "radioberry/device.hpp"
#include "radioberry/node.hpp"

namespace radioberry::ctl {

enum ExitCode : int {
    kExitOk = 0,
    kExitDeviceError = 1,
    kExitUsage = 2,
};

enum class ReportFormat { kText, kJson, kYaml };

struct Options {
    std::string device = kDevicePath;
    // Nothing: report info and statistics only.
    std::optional<Command> command;
    bool allow_tx = false;
    ReportFormat format = ReportFormat::kText;
};

struct ShowHelp {};
struct ShowVersion {};
struct UsageError {
    std::string message;
};

using ParseResult = std::variant<Options, ShowHelp, ShowVersion, UsageError>;

ParseResult parse_options(std::span<const std::string> args);

// Why `options` may not be sent, or nothing: a command with transmit
// hazards without --allow-tx, or one that disables the TX watchdog.
std::optional<std::string> refusal(const Options& options);

// The report: the command and reply (when one was sent), the driver's info
// and its statistics.
Node build_report(const Options& options, const std::optional<Reply>& reply,
                  const radioberry_info& info, const radioberry_stats& stats);

struct Environment {
    std::function<std::unique_ptr<Device>(const std::string& path)> open;
    std::ostream& out;
    std::ostream& err;
};

int run_app(std::span<const std::string> args, Environment& env);

extern const char* const kUsage;

}  // namespace radioberry::ctl
