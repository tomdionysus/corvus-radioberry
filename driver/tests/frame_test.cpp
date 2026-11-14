// SPDX-License-Identifier: GPL-2.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "rb_frame.h"
#include "rb_logic.h"

TEST_CASE("CRC-8 is the polynomial 0x07 code with initial value 0") {
    const u8 check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(rb_crc8(check, sizeof(check)) == 0xf4);
    CHECK(rb_crc8(check, 0) == 0x00);
}

TEST_CASE("a frame carries write, space, address, data and sequence") {
    u8 f[RB_FRAME_BYTES];
    rb_frame_build(f, true, true, 0x06, 0x12345678u, 0x9a);
    CHECK(f[0] == 0xc6);
    CHECK(f[1] == 0x12);
    CHECK(f[4] == 0x78);
    CHECK(f[5] == 0x9a);
    CHECK(f[6] == 0x00);
    CHECK(f[7] == rb_crc8(f, 7));
    rb_frame_build(f, false, false, 0x7f, 0, 0);
    CHECK(f[0] == 0x3f);
}

TEST_CASE("a reply is taken only with its header and CRC right") {
    u8 r[RB_FRAME_BYTES] = {0xa5, 0x07, 0xde, 0xad, 0xbe, 0xef, 0x11, 0};
    r[7] = rb_crc8(r, 7);
    u8 status = 0, status2 = 0;
    u32 data = 0;
    REQUIRE(rb_frame_reply(r, &status, &data, &status2));
    CHECK(status == 0x07);
    CHECK(data == 0xdeadbeefu);
    CHECK(status2 == 0x11);
    r[3] ^= 1;
    CHECK_FALSE(rb_frame_reply(r, &status, &data, &status2));
    r[3] ^= 1;
    r[0] = 0x5a;
    r[7] = rb_crc8(r, 7);
    CHECK_FALSE(rb_frame_reply(r, &status, &data, &status2));
}

TEST_CASE("reply byte 0 carries CW or PTT in bit 0 and the jack's key in bit 2") {
    CHECK(rb_frame_reply_flags(0) == 0);
    CHECK(rb_frame_reply_flags(RB_FS2_CW_OR_PTT) == 0x01);
    CHECK(rb_frame_reply_flags(RB_FS2_JACK_KEY) == 0x04);
    CHECK(rb_frame_reply_flags(0xff) == 0x05);
}

TEST_CASE("the image samples the Pi's data half a bit after it arrives") {
    // Measured working points (gateware/docs/linktest-results.md).
    CHECK(rb_i2s_link_reg(2) == (2u << 24 | 1u << 16 | 2u << 8 | 1u));
    CHECK(rb_i2s_link_reg(4) == (1u << 24 | 2u << 16 | 4u << 8 | 1u));
    CHECK(rb_i2s_link_reg(6) == (1u << 24 | 1u << 16 | 6u << 8 | 1u));
    CHECK(rb_i2s_link_reg(8) == (1u << 24 | 0u << 16 | 8u << 8 | 1u));
    CHECK(rb_i2s_link_reg(32) == (0u << 24 | 20u << 16 | 32u << 8 | 1u));
    for (unsigned div = RB_I2S_DIV_MIN; div <= RB_I2S_DIV_MAX; div++) {
        const u32 reg = rb_i2s_link_reg(div);
        CHECK(((reg >> 16) & 0xffu) < div);
        CHECK((reg >> 24) * div + ((reg >> 16) & 0xffu) == (div + 9u) / 2u);
    }
}

TEST_CASE("the flags word carries the tag, the check and the sequence number") {
    const u32 f = rb_i2s_tx_flags(0x12345678u, 0x9abc);
    CHECK((f & 0x80000000u) != 0);
    CHECK(((f >> 24) & 0x7fu) == 0x2au);
    CHECK(((f >> 16) & 0xffu) == (0x12u ^ 0x34u ^ 0x56u ^ 0x78u ^ 0x9au ^ 0xbcu));
    CHECK((f & 0xffffu) == 0x9abcu);
}

TEST_CASE("the transmit schedule sends 48 000 samples a second at every divider") {
    // 1 920 000 frames last `div` seconds at any divider (122.88 MHz / 64 /
    // div frames a second): exactly 48 000 * div samples.
    static u32 in[4096], out[2 * 4096];
    for (unsigned i = 0; i < 4096; i++) in[i] = 0x00010002u * i;
    const unsigned total = RB_I2S_FAST_HZ / 64u;
    for (unsigned div = RB_I2S_DIV_MIN; div <= RB_I2S_DIV_MAX; div++) {
        struct rb_i2s_tx_sched s = {};
        unsigned sent = 0, slots_total = 0, frames = 0;
        while (frames < total) {
            const unsigned n = total - frames < 2000u ? total - frames : 2000u;
            unsigned slots = 0;
            sent += rb_i2s_tx_frames(&s, div, out, n, in, 4096, &slots);
            slots_total += slots;
            frames += n;
        }
        CHECK(slots_total == 48000u * div);
        CHECK(sent == 48000u * div);
        CHECK(s.acc == 0u);
    }
}

TEST_CASE("the transmit schedule numbers samples and sends idle frames when short") {
    static const u32 in[3] = {0x11112222u, 0x33334444u, 0x55556666u};
    u32 out[2 * 20];
    struct rb_i2s_tx_sched s = {};
    unsigned slots = 0;
    // div 20: every second frame is a slot; 10 slots, 3 samples.
    const unsigned used = rb_i2s_tx_frames(&s, 20, out, 20, in, 3, &slots);
    CHECK(used == 3);
    CHECK(slots == 10);
    CHECK(out[0] == 0);
    CHECK(out[1] == 0);
    CHECK(out[2] == 0x11112222u);
    CHECK(out[3] == rb_i2s_tx_flags(0x11112222u, 0));
    CHECK(out[7] == rb_i2s_tx_flags(0x33334444u, 1));
    CHECK(out[11] == rb_i2s_tx_flags(0x55556666u, 2));
    CHECK(out[15] == 0);
    CHECK(s.seq == 3);
}

TEST_CASE("the bit clock is the slowest that carries the stream with a quarter spare") {
    for (unsigned rate = 0; rate < 4; rate++) {
        for (unsigned rx = 1; rx <= 8; rx++) {
            const unsigned div = rb_i2s_div(rate, rx);
            const unsigned need = (48000u << rate) * 2u * rx;
            CHECK(div >= RB_I2S_DIV_MIN);
            CHECK(div <= RB_I2S_DIV_MAX);
            if (need * 5u / 4u <= RB_I2S_WORDS_PER_S(RB_I2S_DIV_MIN)) {
                CHECK(RB_I2S_WORDS_PER_S(div) * 4u >= need * 5u);
                if (div < RB_I2S_DIV_MAX) CHECK(RB_I2S_WORDS_PER_S(div + 1) * 4u < need * 5u);
            } else {
                CHECK(div == RB_I2S_DIV_MIN);
            }
        }
    }
    CHECK(rb_i2s_div(0, 1) == 32);
    CHECK(RB_I2S_VALUES_MAX == 1536000u);
}

TEST_CASE("a DMA period is about 2 ms of words, 256 to 4096") {
    CHECK(rb_i2s_period_words(2) == 4096);
    CHECK(rb_i2s_period_words(32) == 256);
    CHECK(rb_i2s_period_words(255) == 256);
    CHECK(rb_i2s_period_words(8) == 1024);
}

TEST_CASE("idle words are dropped and the valid flag cleared") {
    const u32 in[] = {0x80000001u, 0x00000000u, 0x81fffffeu, 0x7fffffffu, 0x8b123456u};
    u32 out[5] = {};
    REQUIRE(rb_i2s_values(in, 5, out) == 3);
    CHECK(out[0] == 0x00000001u);
    CHECK(out[1] == 0x01fffffeu);
    CHECK(out[2] == 0x0b123456u);
}
