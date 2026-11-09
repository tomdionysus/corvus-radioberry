// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app.hpp"

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path);
    }
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

bool driver_loaded() { return std::filesystem::exists("/sys/module/radioberry"); }

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    radioberry::load::Environment env{
        .read_file = read_file,
        .driver_loaded = driver_loaded,
        .open_pins = radioberry::open_gpiod_config_pins,
        .out = std::cout,
        .err = std::cerr,
    };
    return radioberry::load::run_app(args, env);
}
