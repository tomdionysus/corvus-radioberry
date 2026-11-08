// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ostream>
#include <span>
#include <string>

#include "source.hpp"

namespace radioberry::probe {

enum ExitCode : int {
    kExitRadioCanRun = 0,
    kExitRadioCannotRun = 1,
    kExitUsage = 2,
};

// The whole command line program, with the system behind `source`.
int run_app(std::span<const std::string> args, Source& source, std::ostream& out,
            std::ostream& err);

}  // namespace radioberry::probe
