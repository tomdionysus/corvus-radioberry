// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/monitor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace radioberry::daemon;
using namespace std::chrono_literals;

TEST_CASE("fault names and descriptions", "[monitor]") {
    CHECK(fault_name(FaultKind::RxResync) == "rx-resync");
    CHECK(fault_name(FaultKind::RxDroppedWords) == "rx-dropped-words");
    CHECK(fault_name(FaultKind::RxOverrun) == "rx-overrun");
    CHECK(fault_name(FaultKind::RxQueueFull) == "rx-queue-full");
    CHECK(fault_name(FaultKind::RxStall) == "rx-stall");
    CHECK(fault_name(FaultKind::TxUnderrun) == "tx-underrun");
    CHECK(fault_name(FaultKind::TxInvalidBlock) == "tx-invalid-block");
    CHECK(fault_name(FaultKind::DeviceError) == "device-error");
    CHECK(fault_name(static_cast<FaultKind>(99)) == "unknown");
    CHECK(describe({FaultKind::RxResync, 3, {}}) == "rx-resync: 3");
    CHECK(describe({FaultKind::DeviceError, 1, "read failed"}) == "device-error: read failed");
}

TEST_CASE("the steady clock moves forward", "[monitor]") {
    const Clock clock = steady_clock();
    const auto a = clock();
    CHECK(clock() >= a);
}

TEST_CASE("the monitor reports every counter that moved", "[monitor]") {
    std::vector<Fault> faults;
    Monitor monitor([&](const Fault& f) { faults.push_back(f); }, 250ms);
    CHECK_FALSE(monitor.last());

    radioberry_stats s{};
    s.rx_resyncs = 5;  // history before the baseline is not reported
    monitor.update(s, 0ms, true);
    CHECK(faults.empty());
    REQUIRE(monitor.last());

    s.rx_frames = 100;
    s.rx_resyncs = 7;
    s.rx_dropped_words = 3;
    s.rx_overrun_frames = 10;
    s.tx_underruns = 1;
    monitor.update(s, 100ms, true);
    REQUIRE(faults.size() == 4);
    CHECK(faults[0].kind == FaultKind::RxResync);
    CHECK(faults[0].count == 2);
    CHECK(faults[1].kind == FaultKind::RxDroppedWords);
    CHECK(faults[1].count == 3);
    CHECK(faults[2].kind == FaultKind::RxOverrun);
    CHECK(faults[2].count == 10);
    CHECK(faults[3].kind == FaultKind::TxUnderrun);
    CHECK(faults[3].count == 1);
    CHECK(monitor.last()->rx_frames == 100);
}

TEST_CASE("the monitor reports a stall once, until frames resume", "[monitor]") {
    std::vector<Fault> faults;
    Monitor monitor([&](const Fault& f) { faults.push_back(f); }, 250ms);
    radioberry_stats s{};
    monitor.update(s, 0ms, true);
    monitor.update(s, 200ms, true);
    CHECK(faults.empty());
    monitor.update(s, 250ms, true);
    REQUIRE(faults.size() == 1);
    CHECK(faults[0].kind == FaultKind::RxStall);
    CHECK(monitor.stalled());
    monitor.update(s, 600ms, true);
    CHECK(faults.size() == 1);

    s.rx_frames = 1;
    monitor.update(s, 700ms, true);
    CHECK_FALSE(monitor.stalled());
    monitor.update(s, 900ms, true);
    CHECK(faults.size() == 1);
    monitor.update(s, 950ms, true);
    CHECK(faults.size() == 2);
}

TEST_CASE("no stall is reported while no frames are expected", "[monitor]") {
    std::vector<Fault> faults;
    Monitor monitor([&](const Fault& f) { faults.push_back(f); }, 250ms);
    radioberry_stats s{};
    monitor.update(s, 0ms, false);
    monitor.update(s, 1000ms, false);
    // Frames expected again: the timer starts from here.
    monitor.update(s, 1100ms, true);
    CHECK(faults.empty());
    CHECK_FALSE(monitor.stalled());
}
