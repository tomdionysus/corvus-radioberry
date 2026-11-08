// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ostream>
#include <span>
#include <string>

#include "radioberry/node.hpp"
#include "source.hpp"

namespace radioberry::probe {

enum ExitCode : int {
    kExitRadioCanRun = 0,
    kExitRadioCannotRun = 1,
    kExitUsage = 2,
};

// The report as text: the sections, then findings one per line and the
// verdict.
std::string render_probe_text(const Node& root);

// The whole command line program, with the system behind `source`.
int run_app(std::span<const std::string> args, Source& source, std::ostream& out,
            std::ostream& err);

}  // namespace radioberry::probe
