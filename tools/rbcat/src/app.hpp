// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <ostream>
#include <span>
#include <string>

#include "radioberry/node.hpp"
#include "stream.hpp"

namespace radioberry::cat {

enum ExitCode : int {
    kExitClean = 0,
    kExitStreamError = 1,
    kExitUsage = 2,
};

// Everything the program touches, so tests substitute fakes.
struct Environment {
    std::function<std::unique_ptr<Device>(const std::string& path)> open;
    Clock& clock;
    std::function<bool()> stop;
    std::ostream& out;
    std::ostream& err;
    bool out_is_terminal = false;
};

// The report document for a finished stream.
Node build_report(const Options& options, const StreamResult& result);
// No faults after settling, frames received, and the frame rate within 1%
// of the configured rate when at least one second was measured.
bool stream_clean(const Options& options, const StreamResult& result);

int run_app(std::span<const std::string> args, Environment& env);

}  // namespace radioberry::cat
