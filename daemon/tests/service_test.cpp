// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/service.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_device.hpp"

using namespace radioberry;
using namespace radioberry::daemon;
using namespace std::chrono_literals;
using radioberry::daemon::test::FakeDevice;

namespace {

struct Faults {
    std::mutex mutex;
    std::vector<Fault> list;
    FaultSink sink() {
        return [this](const Fault& f) {
            const std::lock_guard lock(mutex);
            list.push_back(f);
        };
    }
    std::size_t size() {
        const std::lock_guard lock(mutex);
        return list.size();
    }
};

// Polls `done` for up to two seconds.
bool eventually(const std::function<bool()>& done) {
    for (const auto end = std::chrono::steady_clock::now() + 2s;
         std::chrono::steady_clock::now() < end;) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

DeviceOptions options() {
    DeviceOptions o;
    o.wait_ms = 2;
    o.stats_period = 5ms;
    o.stall_after = 10s;  // not under test unless a test lowers it
    return o;
}

}  // namespace

TEST_CASE("the service streams both ways and leaves the gateware safe", "[service]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    Faults faults;
    DeviceService service(device, faults.sink(), options());
    REQUIRE(service.start());
    REQUIRE(service.start());  // already running
    CHECK(service.running());

    service.control().set(hl2::general(0, 2));
    service.control().set(Command{Command::kRun, 0x09, false, 0x20u << 24 | 1u << 19});
    service.control().set_transmit({true, true, false});
    REQUIRE(eventually([&] { return service.snapshot().receivers == 2; }));
    {
        const std::lock_guard lock(device.mutex);
        device.frames_ready = 50;
    }
    REQUIRE(eventually([&] { return !service.rx().empty(); }));
    std::size_t frames = 0;
    while (const RxBlock* b = service.rx().front()) {
        CHECK(b->receivers == 2);
        frames += b->frames();
        service.rx().pop();
    }
    CHECK(frames <= 50);

    TxBlock* t = service.tx().acquire();
    REQUIRE(t != nullptr);
    t->size = 16;
    service.tx().publish();
    REQUIRE(eventually([&] { return service.snapshot().tx_blocks == 1; }));

    const DeviceSnapshot s = service.snapshot();
    CHECK(s.commands >= 2);
    CHECK(s.rx_blocks >= 1);
    CHECK(s.tx_bytes == 16);
    CHECK(s.driver.has_value());
    CHECK_FALSE(s.failed);
    CHECK_FALSE(s.stalled);
    CHECK_FALSE(s.cw_or_ptt);
    CHECK_FALSE(s.external_cw_key);
    CHECK(s.rx_blocks_dropped == 0);
    CHECK(s.rate_code == 0);

    service.stop();
    CHECK_FALSE(service.running());
    service.stop();  // already stopped
    const std::lock_guard lock(device.mutex);
    REQUIRE_FALSE(device.commands.empty());
    const Command& last = device.commands.back();
    CHECK(last.address == 0x09);
    CHECK(last.data == 0);
    CHECK_FALSE(last.can_transmit());
}

TEST_CASE("the service reports a stall and a failed start", "[service]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    Faults faults;
    DeviceOptions o = options();
    o.stall_after = 20ms;
    {
        DeviceService service(device, faults.sink(), o);
        REQUIRE(service.start());
        service.control().set_run(true);  // frames are expected only with run set
        REQUIRE(eventually([&] { return service.snapshot().stalled; }));
        CHECK(faults.size() == 1);
    }  // the destructor stops it

    FakeDevice broken;
    broken.fail_on = "info";
    Faults more;
    DeviceService service(broken, more.sink(), o);
    CHECK_FALSE(service.start());
    CHECK_FALSE(service.running());
    CHECK(service.failed());
    CHECK(more.size() == 1);
}

TEST_CASE("the service keeps running TX after an RX device error", "[service]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    Faults faults;
    DeviceService service(device, faults.sink(), options());
    REQUIRE(service.start());
    {
        const std::lock_guard lock(device.mutex);
        device.fail_on = "stats";
    }
    REQUIRE(eventually([&] { return service.failed(); }));
    TxBlock* t = service.tx().acquire();
    t->size = 4;
    service.tx().publish();
    REQUIRE(eventually([&] { return service.snapshot().tx_blocks == 1; }));
    service.stop();
    CHECK(faults.size() == 1);
}

TEST_CASE("each device thread reports its start", "[service]") {
    FakeDevice device;
    Faults faults;
    DeviceOptions o = options();
    std::mutex mutex;
    std::vector<std::string> roles;
    o.on_thread_start = [&](const char* role) {
        const std::lock_guard lock(mutex);
        roles.emplace_back(role);
    };
    DeviceService service(device, faults.sink(), o);
    REQUIRE(service.start());
    CHECK(eventually([&] {
        const std::lock_guard lock(mutex);
        return roles.size() == 2;
    }));
    service.stop();
    const std::lock_guard lock(mutex);
    std::sort(roles.begin(), roles.end());
    CHECK(roles == std::vector<std::string>{"rx", "tx"});
}
