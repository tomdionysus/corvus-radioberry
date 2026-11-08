// SPDX-License-Identifier: GPL-2.0-or-later
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "app.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

class SteadyClock final : public radioberry::cat::Clock {
public:
    double now() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

}  // namespace

int main(int argc, char** argv) {
    // No SA_RESTART: a signal interrupts a blocking read so the stream ends
    // promptly and the report is still printed.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::ios::sync_with_stdio(false);
    const std::vector<std::string> args(argv + 1, argv + argc);
    SteadyClock clock;
    radioberry::cat::Environment env{
        .open = radioberry::open_device,
        .clock = clock,
        .stop = [] { return g_stop.load(); },
        .out = std::cout,
        .err = std::cerr,
        .out_is_terminal = isatty(STDOUT_FILENO) == 1,
    };
    return radioberry::cat::run_app(args, env);
}
