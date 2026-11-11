// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/control.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using radioberry::Command;
using radioberry::daemon::Control;
using radioberry::daemon::TransmitState;

namespace {

Command reg(std::uint8_t address, std::uint32_t data) {
    Command c;
    c.address = address;
    c.data = data;
    return c;
}

}  // namespace

TEST_CASE("registers go out once each, latest value, in order of first set", "[control]") {
    Control control;
    CHECK_FALSE(control.next());
    control.set(reg(0x02, 7000000));
    control.set(reg(0x00, 0x08));
    control.set(reg(0x02, 14074000));
    CHECK(control.pending() == 2);
    CHECK(control.value(0x02) == 14074000u);
    CHECK_FALSE(control.value(0x03));
    CHECK_FALSE(control.value(0x40));

    const auto first = control.next();
    REQUIRE(first);
    CHECK(first->address == 0x02);
    CHECK(first->data == 14074000);
    CHECK(first->flags == 0);  // run is clear until set_run(true)
    CHECK_FALSE(first->ptt);
    const auto second = control.next();
    REQUIRE(second);
    CHECK(second->address == 0x00);
    CHECK_FALSE(control.next());
    CHECK(control.pending() == 0);
}

TEST_CASE("registers refuse transmit bits and a disabled TX watchdog", "[control]") {
    Control control;
    CHECK_THROWS_AS(control.set(reg(0x40, 0)), std::invalid_argument);
    Command ptt = reg(0x02, 1);
    ptt.ptt = true;
    CHECK_THROWS_AS(control.set(ptt), std::invalid_argument);
    Command permit = reg(0x02, 1);
    permit.flags |= Command::kTxPermit;
    CHECK_THROWS_AS(control.set(permit), std::invalid_argument);
    Command cwx = reg(0x02, 1);
    cwx.flags = Command::kCwx;
    CHECK_THROWS_AS(control.set(cwx), std::invalid_argument);
    CHECK_THROWS_AS(control.set(reg(0x17, 31u << 8 | 20)), std::invalid_argument);
    CHECK(control.pending() == 0);

    control.set(reg(0x17, 12u << 8 | 20));
    Command no_run = reg(0x03, 1);
    no_run.flags = 0;
    control.set(no_run);
    CHECK(control.pending() == 2);
}

TEST_CASE("the transmit bits and run are stamped on every command", "[control]") {
    Control control;
    CHECK_FALSE(control.run());
    control.set_run(true);
    CHECK(control.run());
    control.set_transmit({true, true, true});
    CHECK(control.transmit() == TransmitState{true, true, true});
    // Nothing has been sent, so there is no register to carry the change.
    CHECK(control.pending() == 0);
    CHECK_FALSE(control.next());

    control.set(reg(0x01, 14074000));
    auto c = control.next();
    REQUIRE(c);
    CHECK(c->ptt);
    CHECK(c->flags == (Command::kRun | Command::kTxPermit | Command::kCwx));

    control.set_run(false);
    CHECK_FALSE(control.run());
    control.set_transmit({false, true, false});
    CHECK(control.pending() == 1);  // the carrier
    c = control.next();
    REQUIRE(c);
    CHECK(c->address == 0x01);  // the last register sent, again
    CHECK(c->data == 14074000);
    CHECK_FALSE(c->ptt);
    CHECK(c->flags == Command::kTxPermit);
    CHECK_FALSE(control.next());

    // Setting the same state again changes nothing.
    control.set_transmit({false, true, false});
    control.set_run(false);
    CHECK_FALSE(control.next());
}

TEST_CASE("safe clears the transmit bits and sends 0x09 first, hazards cleared", "[control]") {
    Control control;
    control.set_run(true);
    control.set(reg(0x02, 14074000));
    control.next();
    control.set(reg(0x00, 0x08));
    control.set(reg(0x03, 7074000));
    // Drive 255, VNA, PA enable, T/R relay disable.
    control.set(reg(0x09, 0xffu << 24 | 1u << 23 | 1u << 19 | 1u << 18));
    control.set_transmit({true, true, false});
    control.safe();
    CHECK(control.transmit() == TransmitState{});

    const auto c = control.next();
    REQUIRE(c);
    CHECK(c->address == 0x09);
    CHECK(c->data == 1u << 18);
    CHECK_FALSE(c->ptt);
    CHECK(c->flags == Command::kRun);
    CHECK(control.next()->address == 0x00);
    CHECK(control.next()->address == 0x03);
    CHECK_FALSE(control.next());

    // Twice in a row, and with 0x09 never set: one command, data 0.
    Control fresh;
    fresh.safe();
    fresh.safe();
    CHECK(fresh.pending() == 1);
    const auto d = fresh.next();
    REQUIRE(d);
    CHECK(d->address == 0x09);
    CHECK(d->data == 0);
    CHECK_FALSE(fresh.next());
}
