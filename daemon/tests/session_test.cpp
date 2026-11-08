// SPDX-License-Identifier: GPL-2.0-or-later
#include "session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace radioberry::daemon;
using namespace std::chrono_literals;

namespace {

const Owner kA{Protocol::P1, {0xc0000201, 50000, false}};
const Owner kB{Protocol::P1, {0xc0000202, 50000, true}};
const Owner kP2{Protocol::P2, {0xc0000201, 50000, false}};

struct Rig {
    int safe = 0;
    Session session{[this] { ++safe; }, 3s};
};

}  // namespace

TEST_CASE("peers and owners are described", "[session]") {
    CHECK(describe(kA.peer) == "192.0.2.1:50000 (UDP)");
    CHECK(describe(kB) == "Protocol 1 client 192.0.2.2:50000 (TCP)");
    CHECK(describe(kP2) == "Protocol 2 client 192.0.2.1:50000 (UDP)");
}

TEST_CASE("Protocol 2 discovery is recognised", "[session]") {
    std::vector<std::uint8_t> p(60, 0);
    p[4] = 0x02;
    CHECK(is_p2_discovery(p));
    p[4] = 0x00;
    CHECK_FALSE(is_p2_discovery(p));
    p[4] = 0x02;
    p[0] = 0xef;
    CHECK_FALSE(is_p2_discovery(p));
    p[0] = 0;
    p.push_back(0);
    CHECK_FALSE(is_p2_discovery(p));
}

TEST_CASE("the first claim owns the radio; others wait", "[session]") {
    Rig rig;
    CHECK_FALSE(rig.session.owner());
    CHECK_FALSE(rig.session.busy_for(kA));
    CHECK(rig.session.claim(kA, 0s));
    CHECK(rig.session.claim(kA, 1s));  // again: still the owner
    CHECK(rig.session.owns(kA));
    CHECK(*rig.session.owner() == kA);
    CHECK_FALSE(rig.session.claim(kB, 1s));
    CHECK_FALSE(rig.session.claim(kP2, 1s));  // same address, other protocol
    CHECK(rig.session.busy_for(kB));
    CHECK_FALSE(rig.session.busy_for(kA));
    CHECK_FALSE(rig.session.owns(kB));

    // Only the owner can release it, and releasing makes the gateware safe.
    rig.session.release(kB, "not the owner");
    CHECK(rig.session.owns(kA));
    CHECK(rig.safe == 0);
    rig.session.release(kA, "done");
    CHECK_FALSE(rig.session.owner());
    CHECK(rig.safe == 1);
    CHECK(rig.session.claim(kB, 2s));
}

TEST_CASE("a silent owner is released after the timeout", "[session]") {
    Rig rig;
    CHECK_FALSE(rig.session.tick(10s));  // no owner
    rig.session.claim(kA, 0s);
    rig.session.heard(kB, 2s);  // not the owner: ignored
    CHECK_FALSE(rig.session.tick(2900ms));
    rig.session.heard(kA, 2s);
    CHECK_FALSE(rig.session.tick(4900ms));
    const auto gone = rig.session.tick(5s);
    REQUIRE(gone);
    CHECK(*gone == kA);
    CHECK(rig.safe == 1);
    CHECK(rig.session.claim(kB, 5s));
}

TEST_CASE("MOX is the owner's, and a fault while transmitting latches", "[session]") {
    Rig rig;
    CHECK_FALSE(rig.session.mox(kA, true));  // nobody owns the radio
    rig.session.claim(kA, 0s);
    CHECK(rig.session.mox(kA, true));

    rig.session.fault("sequence gap");
    CHECK(rig.session.latched());
    CHECK(rig.safe == 1);
    CHECK(rig.session.faults() == 1);
    CHECK_FALSE(rig.session.mox(kA, true));  // held: refused
    rig.session.fault("again");              // no MOX now: counted only
    CHECK(rig.safe == 1);
    CHECK(rig.session.faults() == 2);
    CHECK_FALSE(rig.session.mox(kA, false));  // released: latch cleared
    CHECK_FALSE(rig.session.latched());
    CHECK(rig.session.mox(kA, true));

    // Losing the owner clears MOX and the latch.
    rig.session.fault("gap");
    rig.session.release(kA, "done");
    CHECK_FALSE(rig.session.latched());
    rig.session.claim(kB, 1s);
    CHECK_FALSE(rig.session.mox(kA, true));  // no longer the owner
    CHECK(rig.session.mox(kB, true));
}
