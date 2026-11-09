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

// Parses the command line (without the program name). With `--config
// PATH`, the YAML file there is read first, as if its options came before
// the others, so the command line overrides it. Throws
// std::invalid_argument with a message that names the problem.
Options parse_options(const std::vector<std::string>& args);

// The options a config file stands for: each key is a long option with
// `_` for `-` (`frequency_ppm: 1.5` is `--frequency-ppm 1.5`); `allow_tx`,
// `allow_pa` and `realtime` are true or false. An empty file is no options.
// Throws std::invalid_argument for an unreadable file, an unknown key, a
// value that is not a scalar, or a switch that is not true or false.
std::vector<std::string> config_args(const std::string& path);

std::string usage();

}  // namespace radioberry::daemon
