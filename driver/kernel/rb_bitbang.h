/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The RX and TX buses clocked by the CPU, for boards without a PIO (the
 * Raspberry Pi 4). The same handshakes as the PIO programs in rb_pio.h,
 * step for step (gateware/README.md sections 5 and 7):
 *
 * RX: raise and drop the clock, then read the pins. Sample-ready high
 *     marks a start; the next seven clocks each give a nibble, the value's
 *     frame index first, then 24 data bits most significant first. The
 *     result is the word the PIO program pushes: index << 24 | value.
 * TX: read ready, then raise the clock: if ready was high, that edge
 *     starts a word. Then 32 bits, most significant first, each set up
 *     while the clock is low and taken on its rising edge. The gateware's
 *     ready pin is the very signal its shifter starts on, registered on
 *     this clock, so the read cannot disagree with the gateware.
 *
 * Register access is supplied by the includer, which defines struct
 * rb_bb_io and these before including this file:
 *
 *   static inline void rb_bb_set(struct rb_bb_io *io, u32 mask);   GPSET0
 *   static inline void rb_bb_clr(struct rb_bb_io *io, u32 mask);   GPCLR0
 *   static inline u32 rb_bb_lev(struct rb_bb_io *io);              GPLEV0
 *
 * so the kernel, the userspace bus tool and the gateware simulator in the
 * tests run this same code with no indirect calls. No kernel dependencies.
 */
#ifndef RB_BITBANG_H
#define RB_BITBANG_H

#include "rb_logic.h"

/* Raspberry Pi GPIO numbers, as in rb_pio.h. */
#define RB_BB_TX_CLK (1u << 4)
#define RB_BB_TX_DATA (1u << 5)
#define RB_BB_RX_CLK (1u << 6)
#define RB_BB_TX_READY (1u << 12)
#define RB_BB_RX_D0_SHIFT 18 /* D0..D3 on 18..21 */
#define RB_BB_RX_READY (1u << 25)

#define RB_BB_RX_NIBBLES 7 /* index, then six data nibbles */

struct rb_bb_stats {
	u64 rx_clocks;      /* RX clock cycles given */
	u64 rx_idle_clocks; /* of those, with sample-ready low */
	u64 rx_words;
	u64 tx_clocks;      /* TX clock cycles given */
	u64 tx_ready_waits; /* polling edges with ready low */
	u64 tx_words;
};

/*
 * Extra level reads after each clock pulse before the pins are taken, to
 * give the gateware's outputs time to settle if a single read follows the
 * edge too closely. No setting measured needed it (docs/testing.md).
 */
struct rb_bb_bus {
	struct rb_bb_io *io;
	unsigned int settle_reads;
	struct rb_bb_stats stats;
};

/* One clock pulse on @clk, then the pin levels. */
static inline u32 rb_bb_pulse(struct rb_bb_bus *b, u32 clk)
{
	unsigned int i;

	rb_bb_set(b->io, clk);
	rb_bb_clr(b->io, clk);
	for (i = 0; i < b->settle_reads; i++)
		(void)rb_bb_lev(b->io);
	return rb_bb_lev(b->io);
}

/*
 * Clocks the RX bus until @max_words words are in @out or @max_idle
 * clocks in a row found sample-ready low (the gateware's FIFO holds 12 or
 * fewer values). Returns the number of words stored. The clock is low on
 * return, so the bus is still.
 */
static inline unsigned int rb_bb_rx_run(struct rb_bb_bus *b, u32 *out, unsigned int max_words,
					unsigned int max_idle)
{
	unsigned int words = 0, idle = 0, i;
	u32 lev, word;

	while (words < max_words && idle < max_idle) {
		lev = rb_bb_pulse(b, RB_BB_RX_CLK);
		b->stats.rx_clocks++;
		if (!(lev & RB_BB_RX_READY)) {
			b->stats.rx_idle_clocks++;
			idle++;
			continue;
		}
		idle = 0;
		word = 0;
		for (i = 0; i < RB_BB_RX_NIBBLES; i++) {
			lev = rb_bb_pulse(b, RB_BB_RX_CLK);
			word = (word << 4) | ((lev >> RB_BB_RX_D0_SHIFT) & 0xfu);
		}
		b->stats.rx_clocks += RB_BB_RX_NIBBLES;
		out[words++] = word;
	}
	b->stats.rx_words += words;
	return words;
}

/*
 * Sends up to @count TX words, I in bits 31..16 and Q in 15..0. Before
 * each word, at most @max_polls polling edges wait for ready. Returns the
 * number of words sent, fewer when the gateware's FIFO stayed full.
 */
static inline unsigned int rb_bb_tx_run(struct rb_bb_bus *b, const u32 *words, unsigned int count,
					unsigned int max_polls)
{
	unsigned int sent, polls;
	bool ready;
	u32 word;
	int bit;

	for (sent = 0; sent < count; sent++) {
		ready = false;
		for (polls = 0; polls < max_polls && !ready; polls++) {
			ready = rb_bb_lev(b->io) & RB_BB_TX_READY;
			rb_bb_set(b->io, RB_BB_TX_CLK); /* the start edge if ready */
			rb_bb_clr(b->io, RB_BB_TX_CLK);
			b->stats.tx_clocks++;
			if (!ready)
				b->stats.tx_ready_waits++;
		}
		if (!ready)
			break;
		word = words[sent];
		for (bit = 31; bit >= 0; bit--) {
			if ((word >> bit) & 1u)
				rb_bb_set(b->io, RB_BB_TX_DATA);
			else
				rb_bb_clr(b->io, RB_BB_TX_DATA);
			rb_bb_set(b->io, RB_BB_TX_CLK);
			rb_bb_clr(b->io, RB_BB_TX_CLK);
		}
		b->stats.tx_clocks += 32;
	}
	b->stats.tx_words += sent;
	return sent;
}

#endif /* RB_BITBANG_H */
