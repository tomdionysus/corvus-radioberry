// SPDX-License-Identifier: GPL-2.0-or-later
//
// radioberryd. Process glue: options, logging, signals; the work is in
// Daemon, which the tests run against a fake device.
#include <spdlog/sinks/systemd_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
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
    // Under systemd stderr is the journal: log to it directly, with each
    // message's priority and the journal's own timestamps.
    if (std::getenv("JOURNAL_STREAM") != nullptr) {
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "radioberryd", std::make_shared<spdlog::sinks::systemd_sink_mt>("radioberryd")));
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
