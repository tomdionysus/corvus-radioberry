// SPDX-License-Identifier: GPL-2.0-or-later
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "app.hpp"
#include "live_source.hpp"

int main(int argc, char** argv) {
    spdlog::set_default_logger(spdlog::stderr_color_st("rbprobe"));
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        radioberry::probe::LiveSource source;
        return radioberry::probe::run_app(args, source, std::cout, std::cerr);
    } catch (const std::exception& e) {
        std::cerr << "rbprobe: " << e.what() << '\n';
        return radioberry::probe::kExitUsage;
    }
}
