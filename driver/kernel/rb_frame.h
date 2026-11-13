/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The framed control protocol of the gateware-v2 images and the I2S word
 * format of the Raspberry Pi 4 receive image, as pure logic with no kernel
 * dependencies so it is tested in userspace (driver/tests/frame_test.cpp).
 * The gateware side is gateware-v2/rtl/common/rbv_spi.sv.
 *
 * A frame is 8 bytes each way, SPI mode 3, most significant bit first.
 *   Host to FPGA:  byte 0  bit 7 write, bit 6 space (0 radio, 1 board),
 *                          bits 5..0 address
 *                  1..4    data, most significant byte first
 *                  5       sequence number
 *                  6       0
 *                  7       CRC-8 (polynomial 0x07, initial 0) of bytes 0..6
 *   FPGA to host:  byte 0  0xa5
 *                  1       status (RB_FS_*)
 *                  2..5    the addressed register's value before any write
 *                  6       sequence number of the last frame accepted
 *                  7       CRC-8 of bytes 0..6
 * Radio space carries Hermes-Lite 2 commands (address, data); board space
 * the image's own registers (RB_BREG_*).
 */
#ifndef RB_FRAME_H
#define RB_FRAME_H

#include "rb_logic.h" /* the kernel types, in userspace builds too */

#define RB_FRAME_BYTES 8
#define RB_FRAME_HEADER 0xa5

/* Status byte. */
#define RB_FS_PLL_LOCKED (1u << 0)
#define RB_FS_SET_UP (1u << 1)	  /* AD9866 programmed */
#define RB_FS_RUNNING (1u << 2)	  /* radio out of reset */
#define RB_FS_I2S (1u << 3)
#define RB_FS_FIFO_FULL (1u << 4) /* sticky */
#define RB_FS_ADC_FULL (1u << 5)  /* sticky */
#define RB_FS_BAD_CRC (1u << 6)	  /* sticky */

/* Board registers. */
#define RB_BREG_ID 0x00
#define RB_BREG_VERSION 0x01
#define RB_BREG_CAPS 0x02
#define RB_BREG_STATUS 0x03
#define RB_BREG_CONTROL 0x04 /* bit 0 run, bit 1 restart */
#define RB_BREG_KEYING 0x05  /* bit 0 PTT, bit 1 transmit permission */
#define RB_BREG_I2S 0x06     /* 0 enable, 15..8 divider, 23..16 phase, 29..24 offset */
#define RB_BREG_CRC_ERR 0x07
#define RB_BREG_FRAMES 0x08
#define RB_BREG_FIFO_FULL 0x09

#define RB_ID_RBV2 0x52425632u

/* CAPS fields. */
#define RB_CAPS_RECEIVERS(c) ((c) & 0xfu)
#define RB_CAPS_TX(c) (((c) >> 4) & 1u)
#define RB_CAPS_TRANSPORT(c) (((c) >> 8) & 0xfu)
#define RB_CAPS_FPGA(c) (((c) >> 12) & 0xfu)
#define RB_CAPS_TRANSPORT_I2S 1

/* The I2S link: bit clock = RB_I2S_FAST_HZ / divider, 32 bits a word. */
#define RB_I2S_FAST_HZ 122880000u
#define RB_I2S_WORDS_PER_S(div) (RB_I2S_FAST_HZ / 32u / (div))
#define RB_I2S_DIV_MIN 2u
#define RB_I2S_DIV_MAX 255u
#define RB_I2S_VALID 0x80000000u
/* What the link carries at its fastest, with a quarter kept spare. */
#define RB_I2S_VALUES_MAX (RB_I2S_WORDS_PER_S(RB_I2S_DIV_MIN) / 5u * 4u)

static inline u8 rb_crc8(const u8 *p, unsigned int n)
{
	u8 c = 0;
	unsigned int i, b;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (b = 0; b < 8; b++)
			c = (u8)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
	}
	return c;
}

static inline void rb_frame_build(u8 *out, bool write, bool board, u8 addr, u32 data, u8 seq)
{
	out[0] = (u8)((write ? 0x80 : 0) | (board ? 0x40 : 0) | (addr & 0x3f));
	out[1] = (u8)(data >> 24);
	out[2] = (u8)(data >> 16);
	out[3] = (u8)(data >> 8);
	out[4] = (u8)data;
	out[5] = seq;
	out[6] = 0;
	out[7] = rb_crc8(out, 7);
}

/* A reply: false unless its header and CRC are right. */
static inline bool rb_frame_reply(const u8 *in, u8 *status, u32 *data, u8 *seq_last)
{
	if (in[0] != RB_FRAME_HEADER || rb_crc8(in, 7) != in[7])
		return false;
	*status = in[1];
	*data = ((u32)in[2] << 24) | ((u32)in[3] << 16) | ((u32)in[4] << 8) | (u32)in[5];
	*seq_last = in[6];
	return true;
}

/*
 * The slowest bit clock that carries receivers at a rate code with a
 * quarter spare: the divider, RB_I2S_DIV_MIN to RB_I2S_DIV_MAX.
 */
static inline unsigned int rb_i2s_div(unsigned int rate_code, unsigned int receivers)
{
	u32 need = (48000u << (rate_code & 3)) * 2u * (receivers ? receivers : 1);
	u32 div = (RB_I2S_FAST_HZ / 32u) * 4u / (need * 5u);

	if (div < RB_I2S_DIV_MIN)
		div = RB_I2S_DIV_MIN;
	if (div > RB_I2S_DIV_MAX)
		div = RB_I2S_DIV_MAX;
	return div;
}

/* Words a DMA period holds: about 2 ms at the divider, 256 to 4096. */
static inline unsigned int rb_i2s_period_words(unsigned int div)
{
	u32 words = RB_I2S_WORDS_PER_S(div) / 500u;
	unsigned int p = 256;

	while (p < words && p < 4096)
		p <<= 1;
	return p;
}

/*
 * Keeps the words that carry a value, as the RX parser takes them
 * (index << 24 | value). Returns how many were kept.
 */
static inline unsigned int rb_i2s_values(const u32 *in, unsigned int n, u32 *out)
{
	unsigned int i, k = 0;

	for (i = 0; i < n; i++)
		if (in[i] & RB_I2S_VALID)
			out[k++] = in[i] & 0x0fffffffu;
	return k;
}

#endif /* RB_FRAME_H */
