// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/rx_pump.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "fake_device.hpp"

using namespace radioberry;
using namespace radioberry::daemon;
using namespace std::chrono_literals;
using radioberry::daemon::test::FakeDevice;

namespace {

struct Rig {
    FakeDevice device;
    Control control;
    SpscRing<RxBlock> ring{4};
    std::vector<Fault> faults;
    std::chrono::nanoseconds now{0};
    Monitor monitor{[this](const Fault& f) { faults.push_back(f); }, 250ms};
    RxPump pump{device,
                control,
                ring,
                monitor,
                [this](const Fault& f) { faults.push_back(f); },
                [this] { return now; },
                {100ms, 0}};
};

}  // namespace

TEST_CASE("the RX pump starts from the driver's layout", "[rx]") {
    Rig rig;
    rig.device.info_.active_receivers = 3;
    rig.device.info_.rx_rate_code = 2;
    REQUIRE(rig.pump.start());
    CHECK(rig.pump.receivers() == 3);
    CHECK(rig.pump.rate_code() == 2);
    CHECK(rig.device.stats_calls == 1);  // the monitor's baseline
    CHECK_FALSE(rig.pump.failed());
}

TEST_CASE("the RX pump reads whole frames into blocks", "[rx]") {
    Rig rig;
    REQUIRE(rig.pump.start());
    rig.device.frames_ready = 10;
    REQUIRE(rig.pump.step());
    const RxBlock* block = rig.ring.front();
    REQUIRE(block != nullptr);
    CHECK(block->size == 60);
    CHECK(block->frames() == 10);
    CHECK(block->receivers == 1);
    CHECK(block->rate_code == 0);
    CHECK(block->sequence == 1);
    CHECK(block->bytes[0] == 0);
    CHECK(block->bytes[59] == 9);
    CHECK(rig.pump.blocks() == 1);

    // Nothing ready: no block.
    rig.ring.pop();
    REQUIRE(rig.pump.step());
    CHECK(rig.ring.front() == nullptr);
    // Readable but the read finds nothing (another reader, a signal).
    rig.device.always_readable = true;
    REQUIRE(rig.pump.step());
    CHECK(rig.ring.front() == nullptr);
    CHECK(rig.faults.empty());
}

TEST_CASE("a block never holds more than its capacity in whole frames", "[rx]") {
    Rig rig;
    rig.device.info_.active_receivers = 5;  // 30-byte frames
    REQUIRE(rig.pump.start());
    rig.device.frames_ready = 10000;
    REQUIRE(rig.pump.step());
    const RxBlock* block = rig.ring.front();
    REQUIRE(block != nullptr);
    CHECK(block->size == RxBlock::kCapacity / 30 * 30);
    CHECK(block->frames() == RxBlock::kCapacity / 30);
}

TEST_CASE("commands go out before the read, and the layout follows them", "[rx]") {
    Rig rig;
    REQUIRE(rig.pump.start());
    rig.device.frames_ready = 4;
    rig.control.set(hl2::rx_frequency(0, 14074000));
    rig.control.set(hl2::general(1, 3));
    rig.device.reply.cw_or_ptt = true;
    rig.device.reply.external_cw_key = true;
    REQUIRE(rig.pump.step());
    REQUIRE(rig.device.commands.size() == 2);
    CHECK(rig.device.commands[0].address == 0x02);
    CHECK(rig.device.commands[1].address == 0x00);
    CHECK(rig.pump.commands() == 2);
    CHECK(rig.pump.receivers() == 3);
    CHECK(rig.pump.rate_code() == 1);
    CHECK(rig.pump.cw_or_ptt());
    CHECK(rig.pump.external_cw_key());
    // The driver reset its ring at the change: the old frames are gone.
    CHECK(rig.ring.front() == nullptr);

    rig.device.frames_ready = 2;
    REQUIRE(rig.pump.step());
    const RxBlock* block = rig.ring.front();
    REQUIRE(block != nullptr);
    CHECK(block->receivers == 3);
    CHECK(block->rate_code == 1);
    CHECK(block->size == 36);

    // A count above the gateware's is clamped by the driver.
    rig.control.set(hl2::general(0, 12));
    REQUIRE(rig.pump.flush_commands());
    CHECK(rig.pump.receivers() == 6);
}

TEST_CASE("statistics are polled on their period and on an overrun", "[rx]") {
    Rig rig;
    REQUIRE(rig.pump.start());
    CHECK(rig.device.stats_calls == 1);
    rig.now = 50ms;
    rig.pump.step();
    CHECK(rig.device.stats_calls == 1);
    rig.now = 100ms;
    rig.pump.step();
    CHECK(rig.device.stats_calls == 2);

    rig.device.overrun = true;
    rig.device.stats_.rx_overrun_frames = 8;
    rig.now = 120ms;
    rig.pump.step();
    CHECK(rig.device.stats_calls == 3);
    CHECK_FALSE(rig.device.overrun);
    REQUIRE(rig.faults.size() == 1);
    CHECK(rig.faults[0].kind == FaultKind::RxOverrun);
    CHECK(rig.faults[0].count == 8);
}

TEST_CASE("a full ring drops blocks and reports once until it drains", "[rx]") {
    Rig rig;
    REQUIRE(rig.pump.start());
    for (int i = 0; i < 6; ++i) {
        rig.device.frames_ready = 1;
        REQUIRE(rig.pump.step());
    }
    CHECK(rig.ring.size() == 4);
    CHECK(rig.pump.blocks() == 4);
    CHECK(rig.pump.blocks_dropped() == 2);
    REQUIRE(rig.faults.size() == 1);
    CHECK(rig.faults[0].kind == FaultKind::RxQueueFull);

    // The consumer catches up; the sequence shows the gap.
    for (int i = 0; i < 4; ++i) {
        rig.ring.pop();
    }
    rig.device.frames_ready = 1;
    REQUIRE(rig.pump.step());
    CHECK(rig.ring.front()->sequence == 7);
    rig.ring.pop();
    for (int i = 0; i < 5; ++i) {
        rig.device.frames_ready = 1;
        REQUIRE(rig.pump.step());
    }
    CHECK(rig.faults.size() == 2);
}

TEST_CASE("a device error is reported once and ends the pump", "[rx]") {
    for (const char* op : {"command", "info", "wait", "stats", "read"}) {
        INFO(op);
        Rig rig;
        REQUIRE(rig.pump.start());
        rig.control.set(hl2::general(0, 2));
        rig.device.always_readable = true;
        rig.now = 100ms;  // statistics due, short of the stall time
        rig.device.fail_on = op;
        CHECK_FALSE(rig.pump.step());
        CHECK(rig.pump.failed());
        REQUIRE(rig.faults.size() == 1);
        CHECK(rig.faults[0].kind == FaultKind::DeviceError);
        CHECK(rig.faults[0].detail.find(op) != std::string::npos);
        CHECK_FALSE(rig.pump.step());
        CHECK_FALSE(rig.pump.flush_commands());
        CHECK(rig.faults.size() == 1);
    }
}

TEST_CASE("start and flush report device errors", "[rx]") {
    Rig rig;
    rig.device.fail_on = "info";
    CHECK_FALSE(rig.pump.start());
    CHECK(rig.pump.failed());

    Rig other;
    REQUIRE(other.pump.start());
    other.control.set(hl2::lna_gain(20));
    other.device.fail_on = "command";
    CHECK_FALSE(other.pump.flush_commands());
    CHECK(other.faults.size() == 1);
}
