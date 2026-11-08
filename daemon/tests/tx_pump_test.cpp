// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/tx_pump.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

#include "fake_device.hpp"

using namespace radioberry;
using namespace radioberry::daemon;
using radioberry::daemon::test::FakeDevice;

namespace {

struct Rig {
    FakeDevice device;
    SpscRing<TxBlock> ring{4};
    std::vector<Fault> faults;
    std::atomic<bool> stop{false};
    TxPump pump{device, ring, [this](const Fault& f) { faults.push_back(f); }, 0};

    void push(std::size_t size, std::uint8_t fill) {
        TxBlock* b = ring.acquire();
        REQUIRE(b != nullptr);
        b->bytes.fill(fill);
        b->size = size;
        ring.publish();
    }
};

}  // namespace

TEST_CASE("the TX pump writes blocks whole and in order", "[tx]") {
    Rig rig;
    rig.push(8, 1);
    rig.push(TxBlock::kCapacity, 2);
    REQUIRE(rig.pump.step(rig.stop));
    REQUIRE(rig.pump.step(rig.stop));
    CHECK(rig.ring.empty());
    CHECK(rig.pump.blocks() == 2);
    CHECK(rig.pump.bytes() == 8 + TxBlock::kCapacity);
    REQUIRE(rig.device.written.size() == 8 + TxBlock::kCapacity);
    CHECK(rig.device.written[7] == 1);
    CHECK(rig.device.written[8] == 2);
    CHECK(rig.faults.empty());
}

TEST_CASE("the TX pump finishes a block over partial writes", "[tx]") {
    Rig rig;
    rig.device.write_limit = 12;
    rig.push(32, 3);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(rig.pump.step(rig.stop));
        CHECK(rig.pump.blocks() == 0);
    }
    CHECK(rig.pump.bytes() == 24);
    REQUIRE(rig.pump.step(rig.stop));
    CHECK(rig.pump.blocks() == 1);
    CHECK(rig.device.written.size() == 32);
    CHECK(rig.ring.empty());
}

TEST_CASE("the TX pump waits while the driver's ring is full", "[tx]") {
    Rig rig;
    rig.device.tx_capacity_words = 2;
    rig.push(16, 4);
    REQUIRE(rig.pump.step(rig.stop));
    CHECK(rig.pump.bytes() == 8);
    REQUIRE(rig.pump.step(rig.stop));  // not writable: nothing happens
    CHECK(rig.pump.bytes() == 8);
    rig.device.drain_tx(2);
    REQUIRE(rig.pump.step(rig.stop));
    CHECK(rig.pump.blocks() == 1);
}

TEST_CASE("the TX pump drops a block that is not whole samples", "[tx]") {
    Rig rig;
    rig.push(6, 5);
    rig.push(TxBlock::kCapacity + 4, 5);
    rig.push(0, 5);  // empty: completes without a write
    REQUIRE(rig.pump.step(rig.stop));
    REQUIRE(rig.pump.step(rig.stop));
    REQUIRE(rig.pump.step(rig.stop));
    CHECK(rig.ring.empty());
    CHECK(rig.device.written.empty());
    REQUIRE(rig.faults.size() == 2);
    CHECK(rig.faults[0].kind == FaultKind::TxInvalidBlock);
    CHECK(rig.faults[0].detail == "TX block of 6 bytes dropped");
    CHECK(rig.pump.blocks() == 1);
}

TEST_CASE("the TX pump blocks on an empty ring until a block or stop", "[tx]") {
    Rig rig;
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        rig.push(4, 6);
    });
    REQUIRE(rig.pump.step(rig.stop));  // waits, then returns
    producer.join();
    REQUIRE(rig.pump.step(rig.stop));
    CHECK(rig.pump.blocks() == 1);

    rig.stop = true;
    CHECK(rig.pump.step(rig.stop));  // returns at once
}

TEST_CASE("a TX device error is reported once and ends the pump", "[tx]") {
    for (const char* op : {"wait", "write"}) {
        Rig rig;
        rig.push(4, 7);
        rig.device.fail_on = op;
        CHECK_FALSE(rig.pump.step(rig.stop));
        CHECK(rig.pump.failed());
        REQUIRE(rig.faults.size() == 1);
        CHECK(rig.faults[0].kind == FaultKind::DeviceError);
        CHECK_FALSE(rig.pump.step(rig.stop));
        CHECK(rig.faults.size() == 1);
    }
}

TEST_CASE("the TX pump drops blocks beyond its ring limit", "[tx]") {
    FakeDevice device;
    SpscRing<TxBlock> ring{4};
    std::vector<Fault> faults;
    std::atomic<bool> stop{false};
    TxPump pump{device, ring, [&](const Fault& f) { faults.push_back(f); }, 0, 8};
    for (int i = 0; i < 3; ++i) {
        TxBlock* b = ring.acquire();
        b->size = 16;  // 4 samples
        ring.publish();
    }
    for (int i = 0; i < 3; ++i) {
        REQUIRE(pump.step(stop));
    }
    CHECK(pump.blocks() == 2);
    CHECK(pump.blocks_dropped() == 1);  // 8 queued + 4 would exceed 8
    CHECK(device.written.size() == 32);
    CHECK(faults.empty());
}
