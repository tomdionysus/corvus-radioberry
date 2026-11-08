// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

#include "daemon.hpp"

namespace radioberry::daemon {

struct Options {
    std::string device = "/dev/radioberry";
    DaemonOptions daemon;
    std::string log_level = "info";  // spdlog level name
    bool help = false;
    bool version = false;
};

// Parses the command line (without the program name). Throws
// std::invalid_argument with a message for the user.
Options parse_options(const std::vector<std::string>& args);

std::string usage();

}  // namespace radioberry::daemon
