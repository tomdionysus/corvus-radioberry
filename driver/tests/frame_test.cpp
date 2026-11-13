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
    u8 status = 0, seq = 0;
    u32 data = 0;
    REQUIRE(rb_frame_reply(r, &status, &data, &seq));
    CHECK(status == 0x07);
    CHECK(data == 0xdeadbeefu);
    CHECK(seq == 0x11);
    r[3] ^= 1;
    CHECK_FALSE(rb_frame_reply(r, &status, &data, &seq));
    r[3] ^= 1;
    r[0] = 0x5a;
    r[7] = rb_crc8(r, 7);
    CHECK_FALSE(rb_frame_reply(r, &status, &data, &seq));
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
