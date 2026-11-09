// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "probe.hpp"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    radioberry::proto::Environment env{
        .open = radioberry::proto::open_udp,
        .now = [] { return std::chrono::steady_clock::now().time_since_epoch(); },
        .out = std::cout,
        .err = std::cerr,
    };
    return radioberry::proto::run_app(radioberry::proto::Protocol::kOne, args, env);
}
