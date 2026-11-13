/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The RX bus read by the BCM2711's Secondary Memory Interface (SMI) on a
 * Raspberry Pi 4. Each SMI read strobe on SOE_N (GPIO 6, the RX clock)
 * is one RX clock, and with 18-bit reads each strobe stores one 32-bit
 * word holding SD0..SD17. The RX bus is on SD10..SD13 (D0..D3, GPIO 18
 * to 21) and SD17 (sample-ready, GPIO 25); the other bits are not ours.
 *
 * SMI strobes at a fixed rate and cannot wait for sample-ready, so most
 * words are idle: the gateware takes a clock with sample-ready low as
 * nothing (gateware/README.md section 5). A word with sample-ready high
 * starts a value; the next seven words each give a nibble, the value's
 * frame index first, then 24 data bits most significant first, and the
 * result is the word rb_bb_rx_run() and the PIO program give:
 * index << 24 | value. The parser keeps its place across buffers.
 *
 * No kernel dependencies: the driver, the userspace measurement tool and
 * the tests run this same code.
 */
#ifndef RB_SMI_H
#define RB_SMI_H

#include "rb_logic.h"

#define RB_SMI_D0_SHIFT 10 /* SD10..SD13 */
#define RB_SMI_READY (1u << 17) /* SD17 */
#define RB_SMI_NIBBLES 7 /* index, then six data nibbles */
#define RB_SMI_STROBES_PER_VALUE (1 + RB_SMI_NIBBLES)

struct rb_smi_parser {
	u8 pending; /* nibbles still to take for the value in progress */
	u32 value;
	u64 strobes; /* words parsed */
	u64 idle; /* of those, between values with sample-ready low */
	u64 values;
};

static inline void rb_smi_init(struct rb_smi_parser *p)
{
	memset(p, 0, sizeof(*p));
}

/*
 * The most values @n words can complete: one per eight words, and one
 * more for a value already in progress.
 */
static inline unsigned int rb_smi_max_values(unsigned int n)
{
	return n / RB_SMI_STROBES_PER_VALUE + 1;
}

/*
 * Parses @n SMI words from @in and stores the values they complete in
 * @out, which holds at least rb_smi_max_values(@n) words. Returns the
 * number stored.
 */
static inline unsigned int rb_smi_parse(struct rb_smi_parser *p, const u32 *in, unsigned int n,
					u32 *out)
{
	unsigned int i, got = 0;
	u32 w;

	for (i = 0; i < n; i++) {
		w = in[i];
		if (p->pending) {
			p->value = (p->value << 4) | ((w >> RB_SMI_D0_SHIFT) & 0xfu);
			if (--p->pending == 0)
				out[got++] = p->value;
		} else if (w & RB_SMI_READY) {
			p->pending = RB_SMI_NIBBLES;
			p->value = 0;
		} else {
			p->idle++;
		}
	}
	p->strobes += n;
	p->values += got;
	return got;
}

#endif /* RB_SMI_H */
