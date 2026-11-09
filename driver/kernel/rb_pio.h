/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PIO programs and state machine register words for the RadioBerry RX and
 * TX streams. Pure data and arithmetic, shared with the unit tests.
 *
 * The two programs are derived from the original Radioberry-2.x Pi 5
 * driver's rb2-rx-stream.c and rb2-tx-stream.c
 * (SBC/rpi-5/device_driver/pio-mode/driver/src/), released into the public
 * domain under the Unlicense. Their handshakes are described in
 * gateware/README.md sections 5 and 7.
 */
#ifndef RB_PIO_H
#define RB_PIO_H

#include "rb_logic.h"

/* Raspberry Pi GPIO numbers (RP1 bank 0). */
#define RB_GPIO_TX_CLK 4
#define RB_GPIO_TX_DATA 5
#define RB_GPIO_RX_CLK 6
#define RB_GPIO_TX_READY 12
#define RB_GPIO_RX_D0 18 /* D0..D3 on 18..21 */
#define RB_GPIO_RX_READY 25

/* RP1 PIO registers follow the RP2040 layout. */
#define RB_PIO_CLKDIV(integer, frac) (((u32)(integer) << 16) | ((u32)(frac) << 8))

#define RB_EXECCTRL_SIDE_EN (1u << 30)
#define RB_EXECCTRL_JMP_PIN(pin) (((u32)(pin) & 0x1f) << 24)
#define RB_EXECCTRL_WRAP_TOP(addr) (((u32)(addr) & 0x1f) << 12)
#define RB_EXECCTRL_WRAP_BOTTOM(addr) (((u32)(addr) & 0x1f) << 7)

#define RB_SHIFTCTRL_AUTOPUSH (1u << 16)
#define RB_SHIFTCTRL_PUSH_THRESH(bits) (((u32)(bits) & 0x1f) << 20)

#define RB_PINCTRL_OUT_BASE(pin) ((u32)(pin) & 0x1f)
#define RB_PINCTRL_SIDESET_BASE(pin) (((u32)(pin) & 0x1f) << 10)
#define RB_PINCTRL_IN_BASE(pin) (((u32)(pin) & 0x1f) << 15)
#define RB_PINCTRL_OUT_COUNT(n) (((u32)(n) & 0x3f) << 20)
#define RB_PINCTRL_SIDESET_COUNT(n) (((u32)(n) & 0x7) << 29)

#define RB_PIO_MAX_INSTRUCTIONS 32

struct rb_pio_program {
	const u16 *instructions;
	u8 length;
	u8 wrap_target;
	u8 wrap;
};

/*
 * RX: toggle the clock (side-set) while sample-ready is low; when it is
 * high, clock and read seven nibbles (metadata and 24 data bits). Autopush
 * at 28 bits, shifting left.
 */
static const u16 rb_rx_instructions[] = {
	0x000f, /*  0: jmp    15                        */
	0xbb42, /*  1: nop                    side 1 [3] */
	0x5004, /*  2: in     pins, 4         side 0    */
	0xbb42, /*  3: nop                    side 1 [3] */
	0x5004, /*  4: in     pins, 4         side 0    */
	0xbb42, /*  5: nop                    side 1 [3] */
	0x5004, /*  6: in     pins, 4         side 0    */
	0xbb42, /*  7: nop                    side 1 [3] */
	0x5004, /*  8: in     pins, 4         side 0    */
	0xbb42, /*  9: nop                    side 1 [3] */
	0x5004, /* 10: in     pins, 4         side 0    */
	0xbb42, /* 11: nop                    side 1 [3] */
	0x5004, /* 12: in     pins, 4         side 0    */
	0xbb42, /* 13: nop                    side 1 [3] */
	0x5004, /* 14: in     pins, 4         side 0    */
	0xba42, /* 15: nop                    side 1 [2] */
	0xb242, /* 16: nop                    side 0 [2] */
	0x00c1, /* 17: jmp    pin, 1                    */
};

/*
 * TX: raise the clock and test ready; while ready is low drop the clock and
 * retry. When ready, the polling edge starts the gateware's shifter; pull a
 * word and shift it out most significant bit first, one bit per clock.
 */
static const u16 rb_tx_instructions[] = {
	0x1ac2, /* 0: jmp    pin, 2          side 1 [2] */
	0x1200, /* 1: jmp    0               side 0 [2] */
	0xb242, /* 2: nop                    side 0 [2] */
	0xe03f, /* 3: set    x, 31                      */
	0x80a0, /* 4: pull   block                      */
	0x6001, /* 5: out    pins, 1                    */
	0xba42, /* 6: nop                    side 1 [2] */
	0x1245, /* 7: jmp    x--, 5          side 0 [2] */
	0x0000, /* 8: jmp    0                          */
};

static const struct rb_pio_program rb_rx_program = {
	.instructions = rb_rx_instructions,
	.length = (u8)ARRAY_SIZE(rb_rx_instructions),
	.wrap_target = 0,
	.wrap = 17,
};

static const struct rb_pio_program rb_tx_program = {
	.instructions = rb_tx_instructions,
	.length = (u8)ARRAY_SIZE(rb_tx_instructions),
	.wrap_target = 0,
	.wrap = 8,
};

/*
 * Programs are handed to rp1_pio_add_program() as written for offset 0;
 * the RP1 firmware relocates jump targets to the load offset (the TX
 * program, loaded second, depends on this). Only the wrap addresses in
 * EXECCTRL are computed here from the returned offset.
 */

struct rb_sm_config {
	u32 clkdiv;
	u32 execctrl;
	u32 shiftctrl;
	u32 pinctrl;
};

/* The original driver's clocking: 200 MHz / 2 for RX, / 8 for TX. */
#define RB_RX_CLKDIV_DEFAULT 2
#define RB_TX_CLKDIV_DEFAULT 8

static inline struct rb_sm_config rb_rx_sm_config(unsigned int offset, unsigned int clkdiv)
{
	struct rb_sm_config c;

	c.clkdiv = RB_PIO_CLKDIV(clkdiv, 0);
	c.execctrl = RB_EXECCTRL_SIDE_EN | RB_EXECCTRL_JMP_PIN(RB_GPIO_RX_READY) |
		     RB_EXECCTRL_WRAP_TOP(offset + rb_rx_program.wrap) |
		     RB_EXECCTRL_WRAP_BOTTOM(offset + rb_rx_program.wrap_target);
	c.shiftctrl = RB_SHIFTCTRL_AUTOPUSH | RB_SHIFTCTRL_PUSH_THRESH(28);
	c.pinctrl = RB_PINCTRL_SIDESET_COUNT(2) | RB_PINCTRL_IN_BASE(RB_GPIO_RX_D0) |
		    RB_PINCTRL_SIDESET_BASE(RB_GPIO_RX_CLK);
	return c;
}

static inline struct rb_sm_config rb_tx_sm_config(unsigned int offset, unsigned int clkdiv)
{
	struct rb_sm_config c;

	c.clkdiv = RB_PIO_CLKDIV(clkdiv, 0);
	c.execctrl = RB_EXECCTRL_SIDE_EN | RB_EXECCTRL_JMP_PIN(RB_GPIO_TX_READY) |
		     RB_EXECCTRL_WRAP_TOP(offset + rb_tx_program.wrap) |
		     RB_EXECCTRL_WRAP_BOTTOM(offset + rb_tx_program.wrap_target);
	c.shiftctrl = 0; /* shift left, pull threshold 32, no autopull */
	c.pinctrl = RB_PINCTRL_SIDESET_COUNT(2) | RB_PINCTRL_OUT_COUNT(1) |
		    RB_PINCTRL_SIDESET_BASE(RB_GPIO_TX_CLK) | RB_PINCTRL_OUT_BASE(RB_GPIO_TX_DATA);
	return c;
}

#endif /* RB_PIO_H */
