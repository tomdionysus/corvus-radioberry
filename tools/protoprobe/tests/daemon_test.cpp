// SPDX-License-Identifier: GPL-2.0-or-later
//
// p1probe and p2probe against radioberryd over loopback, the daemon driving
// the fake device: discovery, streams, and zero-drive MOX with transmit
// allowed and refused.
#include "daemon.hpp"

#include <unistd.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <sstream>
#include <thread>

#include "fake_device.hpp"
#include "probe.hpp"

using namespace radioberry;
using namespace radioberry::proto;
using namespace std::chrono_literals;
using radioberry::daemon::Daemon;
using radioberry::daemon::DaemonOptions;
using radioberry::daemon::test::FakeDevice;

namespace {

constexpr std::uint32_t kLoopback = 0x7f000001;

// A base port for this process, so parallel test runs do not collide;
// Protocol 2 needs its offsets free too.
std::uint16_t base_port() {
    static std::uint16_t next = static_cast<std::uint16_t>(30000 + (::getpid() % 1500) * 20);
    const std::uint16_t port = next;
    next = static_cast<std::uint16_t>(next + 20);
    return port;
}

// The daemon on loopback, with a thread feeding the fake device RX frames
// and draining its TX ring in real time.
struct Running {
    explicit Running(bool allow_tx) : daemon(device, options(allow_tx)) {
        device.idle_wait_ms = 1;
        thread = std::thread([this] { daemon.run(stop); });
        feeder = std::thread([this] { feed(); });
    }
    ~Running() {
        stop = true;
        thread.join();
        feeder.join();
    }

    static DaemonOptions options(bool allow_tx) {
        DaemonOptions o;
        o.address = kLoopback;
        o.port = base_port();
        o.mac = std::array<std::uint8_t, 6>{2, 0, 0, 0, 0, 9};
        o.device.wait_ms = 2;
        o.device.stall_after = 10s;
        o.realtime = false;
        o.tx.allow_tx = allow_tx;
        o.tx.drive_ceiling = 100;
        return o;
    }

    void feed() {
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t fed = 0;
        while (!stop) {
            std::this_thread::sleep_for(1ms);
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const std::lock_guard lock(device.mutex);
            const unsigned rate = 48000u << device.info_.rx_rate_code;
            const auto due = static_cast<std::uint64_t>(seconds * rate);
            device.frames_ready += due - fed;
            fed = due;
            device.drain_tx(device.stats_.tx_ring_words);
        }
    }

    bool ptt_commanded() {
        const std::lock_guard lock(device.mutex);
        for (const Command& c : device.commands) {
            if (c.ptt) {
                return true;
            }
        }
        return false;
    }

    FakeDevice device;
    Daemon daemon;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::thread feeder;
};

struct App {
    std::ostringstream out;
    std::ostringstream err;
    Environment env{
        .open = open_udp,
        .now = [] { return std::chrono::steady_clock::now().time_since_epoch(); },
        .out = out,
        .err = err,
    };
    int run(Protocol p, std::vector<std::string> args) { return run_app(p, args, env); }
};

}  // namespace

TEST_CASE("p1probe streams from radioberryd", "[protoprobe][daemon]") {
    Running r(false);
    App app;
    const std::string port = std::to_string(r.daemon.port());
    CHECK(app.run(Protocol::kOne, {"--port", port, "--receivers", "2", "--rate", "96", "--seconds",
                                   "1", "--tolerance", "25", "--json"}) == kExitOk);
    INFO(app.out.str() << app.err.str());
    CHECK(app.out.str().find("\"pass\": true") != std::string::npos);
    CHECK(app.out.str().find("\"sequence_breaks\": 0") != std::string::npos);
    CHECK_FALSE(r.ptt_commanded());
}

TEST_CASE("p1probe MOX reaches the device only with transmit allowed", "[protoprobe][daemon]") {
    {
        Running refused(false);
        App app;
        CHECK(app.run(Protocol::kOne, {"--port", std::to_string(refused.daemon.port()), "--mox",
                                       "--seconds", "0.5", "--tolerance", "25"}) == kExitOk);
        CHECK_FALSE(refused.ptt_commanded());
    }
    Running allowed(true);
    App app;
    CHECK(app.run(Protocol::kOne, {"--port", std::to_string(allowed.daemon.port()), "--mox",
                                   "--seconds", "0.5", "--tolerance", "25"}) == kExitOk);
    INFO(app.out.str() << app.err.str());
    CHECK(allowed.ptt_commanded());
    // Drive stays 0 throughout: no 0x09 command carries any.
    const std::lock_guard lock(allowed.device.mutex);
    for (const Command& c : allowed.device.commands) {
        if (c.address == 0x09) {
            CHECK((c.data >> 24) == 0);
        }
    }
}

TEST_CASE("p2probe streams from radioberryd and holds MOX", "[protoprobe][daemon]") {
    Running r(true);
    App app;
    const std::string port = std::to_string(r.daemon.port());
    CHECK(app.run(Protocol::kTwo, {"--port", port, "--receivers", "3", "--seconds", "1",
                                   "--tolerance", "25", "--yaml"}) == kExitOk);
    INFO(app.out.str() << app.err.str());
    CHECK(app.out.str().find("sequence_breaks: 0") != std::string::npos);
    CHECK_FALSE(r.ptt_commanded());

    App mox;
    CHECK(mox.run(Protocol::kTwo, {"--port", port, "--mox", "--seconds", "1", "--tolerance", "25",
                                   "--json"}) == kExitOk);
    INFO(mox.out.str() << mox.err.str());
    CHECK(r.ptt_commanded());
    // The TX I/Q stream ran: some TX I/Q packets were sent.
    CHECK(mox.out.str().find("\"tx_iq_packets\": 0") == std::string::npos);
}

TEST_CASE("p1probe sees radioberryd busy with another client", "[protoprobe][daemon]") {
    Running r(false);
    const std::string port = std::to_string(r.daemon.port());
    // The first client streams in the background.
    std::thread first([&] {
        App a;
        a.run(Protocol::kOne, {"--port", port, "--seconds", "1.5", "--tolerance", "100"});
    });
    std::this_thread::sleep_for(500ms);
    App second;
    CHECK(second.run(Protocol::kOne, {"--port", port}) == kExitFailed);
    CHECK(second.out.str().find("streaming to another client") != std::string::npos);
    first.join();
}
