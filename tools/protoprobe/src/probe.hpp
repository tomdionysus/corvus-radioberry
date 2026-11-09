// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <variant>

#include "client.hpp"
#include "radioberry/node.hpp"

namespace radioberry::proto {

enum ExitCode : int {
    kExitOk = 0,
    kExitFailed = 1,
    kExitUsage = 2,
};

enum class Protocol { kOne, kTwo };
enum class ReportFormat { kText, kJson, kYaml };

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 1024;
    bool discover_only = false;
    StreamConfig stream;
    double seconds = 2.0;
    double settle = 0.2;
    // Zero-drive transmit: MOX held with drive 0 and zero samples for the
    // measurement.
    bool mox = false;
    // How far the packet rate may stray from the expected one, percent.
    double tolerance_percent = 2.0;
    ReportFormat format = ReportFormat::kText;
};

struct ShowHelp {};
struct ShowVersion {};
struct UsageError {
    std::string message;
};

using ParseResult = std::variant<Options, ShowHelp, ShowVersion, UsageError>;

ParseResult parse_options(Protocol protocol, std::span<const std::string> args);

struct Environment {
    std::function<std::unique_ptr<Transport>(const std::string& host, std::uint16_t port)> open;
    Clock now;
    std::ostream& out;
    std::ostream& err;
};

// Discovers the radio and, unless discover_only, streams from it for the
// settling time and the measurement, with MOX over the measurement if
// asked. The report says what was seen and lists every failed check.
Node probe(Protocol protocol, const Options& options, Client& client, const Clock& now);

int run_app(Protocol protocol, std::span<const std::string> args, Environment& env);

std::string usage(Protocol protocol);

}  // namespace radioberry::proto
