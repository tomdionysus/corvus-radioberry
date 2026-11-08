// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <string>
#include <vector>

#include "app.hpp"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    radioberry::ctl::Environment env{
        .open = radioberry::open_device,
        .out = std::cout,
        .err = std::cerr,
    };
    return radioberry::ctl::run_app(args, env);
}
