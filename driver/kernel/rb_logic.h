/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pure logic of the RadioBerry driver: command inspection, the RX word
 * parser with metadata resynchronisation and frame assembly, a byte ring,
 * and TX packing. No kernel dependencies, so the same code is compiled
 * into the userspace unit tests (driver/tests).
 *
 * Formats are described in gateware/README.md.
 */
#ifndef RB_LOGIC_H
#define RB_LOGIC_H

#ifdef __KERNEL__
#include <linux/array_size.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define RB_CMD_BYTES 6
#define RB_MAX_RECEIVERS 8
#define RB_VALUE_BYTES 3
#define RB_MAX_FRAME_BYTES (2 * RB_MAX_RECEIVERS * RB_VALUE_BYTES)
#define RB_TX_SAMPLE_BYTES 4

/* Hermes-Lite 2 command addresses the driver looks at. */
#define RB_ADDR_GENERAL 0x00   /* rate in data[25:24], receivers - 1 in data[6:3] */
#define RB_ADDR_PA 0x09        /* drive, PA enable (bit 19), T/R disable (bit 18) */
#define RB_ADDR_TX_LATENCY 0x17 /* latency data[6:0], PTT hang time data[12:8] */
#define RB_PTT_HANG_DISABLED 31

/* ---- commands ------------------------------------------------------- */

static inline u8 rb_cmd_addr(const u8 *tx) { return (u8)(tx[1] >> 1); }

static inline bool rb_cmd_ptt(const u8 *tx) { return tx[1] & 1u; }

static inline u32 rb_cmd_data(const u8 *tx)
{
	return ((u32)tx[2] << 24) | ((u32)tx[3] << 16) | ((u32)tx[4] << 8) | (u32)tx[5];
}

/*
 * Makes a command safe to send. A PTT hang time of 31 disables the
 * gateware's transmit watchdog, so it is lowered to 30. Returns true if
 * the command was changed.
 */
static inline bool rb_cmd_sanitise(u8 *tx)
{
	if (rb_cmd_addr(tx) == RB_ADDR_TX_LATENCY && (tx[4] & 0x1f) == RB_PTT_HANG_DISABLED) {
		tx[4] = (u8)((tx[4] & ~0x1fu) | (RB_PTT_HANG_DISABLED - 1));
		return true;
	}
	return false;
}

/*
 * The command sent on close and on any fault: run, PTT and the TX
 * permission bit clear, PA control word zero (drive 0, PA disabled).
 */
static inline void rb_cmd_safe(u8 *tx)
{
	memset(tx, 0, RB_CMD_BYTES);
	tx[1] = (u8)(RB_ADDR_PA << 1);
}

/* Receiver count and rate code carried by a general settings command. */
static inline bool rb_cmd_general(const u8 *tx, unsigned int *receivers, unsigned int *rate_code)
{
	u32 data;

	if (rb_cmd_addr(tx) != RB_ADDR_GENERAL)
		return false;
	data = rb_cmd_data(tx);
	*receivers = ((data >> 3) & 0xf) + 1;
	*rate_code = (data >> 24) & 0x3;
	return true;
}

struct rb_identity {
	u8 major;
	u8 minor;
	u8 fpga_type;
	u8 receivers;
	u8 transmitters;
	u8 status;
};

static inline void rb_reply_parse(const u8 *rx, struct rb_identity *id)
{
	id->status = rx[0];
	id->fpga_type = rx[3] & 0x3;
	id->receivers = (rx[3] >> 2) & 0xf;
	id->transmitters = (rx[3] >> 6) & 0x3;
	id->major = rx[4];
	id->minor = rx[5];
}

/* The gateware answers 0.255 while it is still in its post-load reset. */
static inline bool rb_identity_valid(const struct rb_identity *id)
{
	return id->major != 0 && (id->fpga_type == 1 || id->fpga_type == 2) &&
	       id->receivers != 0;
}

/* ---- RX ------------------------------------------------------------- */

/*
 * Bytes of RX data per millisecond at a rate code and receiver count, as a
 * DMA period: whole 32-bit words, at least min_bytes, at most max_bytes.
 */
static inline u32 rb_rx_period_bytes(unsigned int rate_code, unsigned int receivers,
				     u32 min_bytes, u32 max_bytes)
{
	u32 rate = 48000u << (rate_code & 3);
	u32 words = (rate * 2u * receivers + 999u) / 1000u;
	u32 bytes = words * 4u;

	if (bytes < min_bytes)
		bytes = min_bytes;
	if (bytes > max_bytes)
		bytes = max_bytes;
	return bytes & ~3u;
}

/*
 * After a receiver-count change, words of the old layout are still in
 * flight (PIO FIFO and DMA periods). They cannot form two consecutive
 * frames of the new layout: with fewer receivers the old index runs past
 * the new frame length after at most one frame, with more it wraps to 0
 * before the first frame completes. So the parser settles until it has
 * seen this many consecutive clean frames, counting what it throws away
 * meanwhile as discarded rather than as resyncs or drops.
 */
#define RB_RX_SETTLE_FRAMES 2

struct rb_rx_parser {
	u8 values_per_frame;
	u8 expected;
	bool synced;
	u8 settle; /* clean frames still to see before frames are delivered */
	u8 frame[RB_MAX_FRAME_BYTES];
	u64 words;
	u64 resyncs;
	u64 dropped;
	u64 discarded; /* words thrown away while settling */
};

static inline unsigned int rb_rx_frame_bytes(const struct rb_rx_parser *p)
{
	return (unsigned int)p->values_per_frame * RB_VALUE_BYTES;
}

static inline void rb_rx_set_layout(struct rb_rx_parser *p, unsigned int receivers)
{
	if (receivers < 1)
		receivers = 1;
	if (receivers > RB_MAX_RECEIVERS)
		receivers = RB_MAX_RECEIVERS;
	p->values_per_frame = (u8)(2 * receivers);
	p->expected = 0;
	p->synced = false;
}

/* A receiver-count change: new layout, then settle (see above). */
static inline void rb_rx_set_receivers(struct rb_rx_parser *p, unsigned int receivers)
{
	rb_rx_set_layout(p, receivers);
	p->settle = RB_RX_SETTLE_FRAMES;
}

/* A fresh stream: nothing of another layout is in flight. */
static inline void rb_rx_init(struct rb_rx_parser *p, unsigned int receivers)
{
	memset(p, 0, sizeof(*p));
	rb_rx_set_layout(p, receivers);
}

/*
 * Feeds one word from the RX state machine: bits 27..24 the value's index
 * in its frame, bits 23..0 the value, bits 31..28 zero. Returns true when
 * the word completed a frame, which is then in p->frame. A word out of
 * sequence discards the partial frame; the parser restarts at the next
 * word with index 0, which may be the offending word itself. While
 * settling, a break restarts the settle count, frames are withheld, and
 * every word thrown away counts as discarded.
 */
static inline bool rb_rx_feed(struct rb_rx_parser *p, u32 word)
{
	u8 index = (u8)((word >> 24) & 0xf);
	bool clean = (word >> 28) == 0;
	u8 *dst;

	p->words++;
	if (p->synced && (!clean || index != p->expected)) {
		if (p->settle) {
			p->discarded += p->expected;
			p->settle = RB_RX_SETTLE_FRAMES;
		} else {
			p->resyncs++;
			p->dropped += p->expected;
		}
		p->synced = false;
	}
	if (!p->synced) {
		if (!clean || index != 0) {
			if (p->settle)
				p->discarded++;
			else
				p->dropped++;
			return false;
		}
		p->synced = true;
		p->expected = 0;
	}

	dst = &p->frame[p->expected * RB_VALUE_BYTES];
	dst[0] = (u8)(word >> 16);
	dst[1] = (u8)(word >> 8);
	dst[2] = (u8)word;
	if (++p->expected < p->values_per_frame)
		return false;
	p->expected = 0;
	if (p->settle) {
		p->settle--;
		p->discarded += p->values_per_frame;
		return false;
	}
	return true;
}

/* ---- ring ----------------------------------------------------------- */

/* A byte ring of power-of-two size. The caller serialises access. */
struct rb_ring {
	u8 *buf;
	u32 size;
	u32 head; /* write position, free running */
	u32 tail; /* read position, free running */
};

static inline void rb_ring_init(struct rb_ring *r, u8 *buf, u32 size)
{
	r->buf = buf;
	r->size = size;
	r->head = 0;
	r->tail = 0;
}

static inline u32 rb_ring_used(const struct rb_ring *r) { return r->head - r->tail; }

static inline u32 rb_ring_free(const struct rb_ring *r) { return r->size - rb_ring_used(r); }

static inline void rb_ring_reset(struct rb_ring *r) { r->tail = r->head; }

/* Copies all of src in, or nothing if it does not fit. */
static inline bool rb_ring_put(struct rb_ring *r, const u8 *src, u32 len)
{
	u32 pos;
	u32 first;

	if (len > rb_ring_free(r))
		return false;
	pos = r->head & (r->size - 1);
	first = r->size - pos;
	if (first > len)
		first = len;
	memcpy(r->buf + pos, src, first);
	memcpy(r->buf, src + first, len - first);
	r->head += len;
	return true;
}

/* Copies up to len bytes out; returns the count. */
static inline u32 rb_ring_get(struct rb_ring *r, u8 *dst, u32 len)
{
	u32 used = rb_ring_used(r);
	u32 pos;
	u32 first;

	if (len > used)
		len = used;
	pos = r->tail & (r->size - 1);
	first = r->size - pos;
	if (first > len)
		first = len;
	memcpy(dst, r->buf + pos, first);
	memcpy(dst + first, r->buf, len - first);
	r->tail += len;
	return len;
}

/* ---- TX ------------------------------------------------------------- */

/*
 * Converts samples as written by userspace (I then Q, 16-bit big-endian)
 * into TX state machine words, I in bits 31..16 and Q in 15..0; the state
 * machine shifts each word out most significant bit first.
 */
static inline void rb_tx_pack(const u8 *in, u32 *out, size_t samples)
{
	size_t i;

	for (i = 0; i < samples; i++, in += RB_TX_SAMPLE_BYTES)
		out[i] = ((u32)in[0] << 24) | ((u32)in[1] << 16) | ((u32)in[2] << 8) | (u32)in[3];
}

#endif /* RB_LOGIC_H */
