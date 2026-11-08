// SPDX-License-Identifier: GPL-2.0-or-later
//
// radioberryd. Process glue: options, logging, signals; the work is in
// Daemon, which the tests run against a fake device.
#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "daemon.hpp"
#include "options.hpp"
#include "radioberry/version.hpp"

namespace {

std::atomic<bool> stop{false};

void on_signal(int) { stop = true; }

}  // namespace

int main(int argc, char** argv) {
    using namespace radioberry::daemon;
    Options options;
    try {
        options = parse_options(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& e) {
        std::cerr << "radioberryd: " << e.what() << "\n\n" << usage();
        return 2;
    }
    if (options.help) {
        std::cout << usage();
        return 0;
    }
    if (options.version) {
        std::cout << "radioberryd " << radioberry::kVersion << "\n";
        return 0;
    }
    spdlog::set_level(spdlog::level::from_str(options.log_level));
    spdlog::info("radioberryd {}", radioberry::kVersion);

    struct sigaction action{};
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    try {
        const auto device = radioberry::open_device_nonblocking(options.device);
        Daemon daemon(*device, options.daemon);
        return daemon.run(stop) ? 0 : 1;
    } catch (const std::exception& e) {
        spdlog::error("{}", e.what());
        return 1;
    }
}
