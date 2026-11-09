// SPDX-License-Identifier: GPL-2.0-or-later
//
// PIO register words built from pin numbers must match the constants the
// original RadioBerry Pi 5 driver used, which are known to work with this
// gateware.
#include <catch2/catch_test_macros.hpp>

#include "rb_pio.h"

namespace {

// The original driver's rb2-rx-stream.c / rb2-tx-stream.c values, with their
// wrap fields replaced as that driver does at run time.
u32 original_execctrl(u32 base, unsigned offset, unsigned wrap_target, unsigned wrap) {
    base &= ~((0x1fu << 12) | (0x1fu << 7));
    return base | (((offset + wrap) & 0x1f) << 12) | (((offset + wrap_target) & 0x1f) << 7);
}

}  // namespace

TEST_CASE("RX state machine configuration matches the original driver", "[driver][pio]") {
    for (unsigned offset : {0u, 9u, 14u}) {
        const rb_sm_config c = rb_rx_sm_config(offset, RB_RX_CLKDIV_DEFAULT);
        CHECK(c.clkdiv == 0x00020000u);
        CHECK(c.execctrl == original_execctrl(0x5901f600u, offset, 0, 17));
        CHECK(c.shiftctrl == 0x01c10000u);
        CHECK(c.pinctrl == 0x40091800u);
    }
}

TEST_CASE("TX state machine configuration matches the original driver", "[driver][pio]") {
    for (unsigned offset : {0u, 18u, 23u}) {
        const rb_sm_config c = rb_tx_sm_config(offset, RB_TX_CLKDIV_DEFAULT);
        CHECK(c.clkdiv == 0x00080000u);
        CHECK(c.execctrl == original_execctrl(0x4c01fb80u, offset, 0, 8));
        CHECK(c.shiftctrl == 0u);
        CHECK(c.pinctrl == 0x40101005u);
    }
}

TEST_CASE("programs fit PIO memory together", "[driver][pio]") {
    CHECK(rb_rx_program.length == 18);
    CHECK(rb_tx_program.length == 9);
    CHECK(rb_rx_program.length + rb_tx_program.length <= RB_PIO_MAX_INSTRUCTIONS);
    CHECK(rb_rx_program.wrap == rb_rx_program.length - 1);
    CHECK(rb_tx_program.wrap == rb_tx_program.length - 1);
    // The RX program's only data instruction reads four pins.
    CHECK(rb_rx_program.instructions[2] == 0x5004);
    // The TX program shifts out one pin per bit.
    CHECK(rb_tx_program.instructions[5] == 0x6001);
}

TEST_CASE("clock divider encoding", "[driver][pio]") {
    CHECK(RB_PIO_CLKDIV(1, 0) == 0x00010000u);
    CHECK(RB_PIO_CLKDIV(8, 0x80) == 0x00088000u);
}
