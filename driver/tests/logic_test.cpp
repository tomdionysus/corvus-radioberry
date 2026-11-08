// SPDX-License-Identifier: GPL-2.0-or-later
//
// The driver's pure logic (driver/kernel/rb_logic.h), compiled in userspace.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "radioberry_ioctl.h"
#include "rb_logic.h"

namespace {

std::array<u8, RB_CMD_BYTES> command(u8 flags, u8 addr, bool ptt, u32 data) {
    return {flags,
            static_cast<u8>((addr << 1) | (ptt ? 1 : 0)),
            static_cast<u8>(data >> 24),
            static_cast<u8>(data >> 16),
            static_cast<u8>(data >> 8),
            static_cast<u8>(data)};
}

u32 rx_word(unsigned index, u32 value) { return (index << 24) | (value & 0xffffff); }

}  // namespace

TEST_CASE("command fields", "[driver][command]") {
    const auto c = command(RADIOBERRY_CMD_RUN, 0x12, true, 0x01020304);
    CHECK(rb_cmd_addr(c.data()) == 0x12);
    CHECK(rb_cmd_ptt(c.data()));
    CHECK(rb_cmd_data(c.data()) == 0x01020304);
    CHECK_FALSE(rb_cmd_ptt(command(0, 0x12, false, 0).data()));
}

TEST_CASE("the safe command clears run, PTT, permission and the PA word", "[driver][command]") {
    std::array<u8, RB_CMD_BYTES> c;
    c.fill(0xff);
    rb_cmd_safe(c.data());
    CHECK(c[0] == 0);
    CHECK(rb_cmd_addr(c.data()) == RB_ADDR_PA);
    CHECK_FALSE(rb_cmd_ptt(c.data()));
    CHECK(rb_cmd_data(c.data()) == 0);
}

TEST_CASE("a PTT hang time of 31 is lowered to 30", "[driver][command]") {
    // Address 0x17: latency in data bits 6..0, hang time in 12..8 (byte 4).
    auto c = command(0, RB_ADDR_TX_LATENCY, false, (31u << 8) | 0x14);
    CHECK(rb_cmd_sanitise(c.data()));
    CHECK(((rb_cmd_data(c.data()) >> 8) & 0x1f) == 30);
    CHECK((rb_cmd_data(c.data()) & 0x7f) == 0x14);

    auto keep_bits = command(0, RB_ADDR_TX_LATENCY, false, (0x7u << 13) | (31u << 8));
    CHECK(rb_cmd_sanitise(keep_bits.data()));
    CHECK(((rb_cmd_data(keep_bits.data()) >> 13) & 0x7) == 0x7);

    auto ok = command(0, RB_ADDR_TX_LATENCY, false, 12u << 8);
    CHECK_FALSE(rb_cmd_sanitise(ok.data()));
    auto other = command(0, 0x16, false, 31u << 8);
    CHECK_FALSE(rb_cmd_sanitise(other.data()));
    CHECK(rb_cmd_data(other.data()) == (31u << 8));
}

TEST_CASE("general command carries receivers and rate", "[driver][command]") {
    unsigned receivers = 0;
    unsigned rate = 0;
    // Rate code 2 (192 ksps) in bits 25..24, 4 receivers (3) in bits 6..3.
    const auto c = command(0, RB_ADDR_GENERAL, false, (2u << 24) | (3u << 3));
    REQUIRE(rb_cmd_general(c.data(), &receivers, &rate));
    CHECK(receivers == 4);
    CHECK(rate == 2);
    CHECK_FALSE(rb_cmd_general(command(0, 0x01, false, 0).data(), &receivers, &rate));
}

TEST_CASE("identity reply", "[driver][identity]") {
    // CL025: 1 transmitter, 6 receivers, type 2, version 75.2.
    const u8 reply[RB_CMD_BYTES] = {0x04, 0, 0, static_cast<u8>((1 << 6) | (6 << 2) | 2), 75, 2};
    rb_identity id{};
    rb_reply_parse(reply, &id);
    CHECK(id.status == 0x04);
    CHECK(id.transmitters == 1);
    CHECK(id.receivers == 6);
    CHECK(id.fpga_type == RADIOBERRY_FPGA_CL025);
    CHECK(id.major == 75);
    CHECK(id.minor == 2);
    CHECK(rb_identity_valid(&id));

    // During the post-load reset the gateware answers 0.255.
    const u8 in_reset[RB_CMD_BYTES] = {0, 0, 0, 0, 0, 0xff};
    rb_reply_parse(in_reset, &id);
    CHECK_FALSE(rb_identity_valid(&id));
    const u8 bad_type[RB_CMD_BYTES] = {0, 0, 0, static_cast<u8>((4 << 2) | 3), 75, 2};
    rb_reply_parse(bad_type, &id);
    CHECK_FALSE(rb_identity_valid(&id));
    const u8 no_rx[RB_CMD_BYTES] = {0, 0, 0, 1, 75, 2};
    rb_reply_parse(no_rx, &id);
    CHECK_FALSE(rb_identity_valid(&id));
}

TEST_CASE("RX period is one millisecond of data, bounded", "[driver][rx]") {
    // 48 ksps, 1 receiver: 96 words, 384 bytes.
    CHECK(rb_rx_period_bytes(0, 1, 64, 32768) == 384);
    // 384 ksps, 6 receivers: 4608 words.
    CHECK(rb_rx_period_bytes(3, 6, 64, 32768) == 18432);
    CHECK(rb_rx_period_bytes(0, 1, 1024, 32768) == 1024);
    CHECK(rb_rx_period_bytes(3, 8, 64, 4096) == 4096);
    CHECK(rb_rx_period_bytes(3, 8, 64, 4098) % 4 == 0);
}

TEST_CASE("RX parser assembles frames from clean words", "[driver][rx]") {
    rb_rx_parser p;
    rb_rx_init(&p, 2);
    CHECK(rb_rx_frame_bytes(&p) == 12);

    const std::vector<u32> frame = {rx_word(0, 0x010203), rx_word(1, 0x040506),
                                    rx_word(2, 0xfffffe), rx_word(3, 0x800000)};
    for (int round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < frame.size(); ++i) {
            const bool done = rb_rx_feed(&p, frame[i]);
            CHECK(done == (i + 1 == frame.size()));
        }
        const std::vector<u8> expected = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                          0xff, 0xff, 0xfe, 0x80, 0x00, 0x00};
        CHECK(std::vector<u8>(p.frame, p.frame + 12) == expected);
    }
    CHECK(p.words == 12);
    CHECK(p.resyncs == 0);
    CHECK(p.dropped == 0);
}

TEST_CASE("RX parser waits for index 0 before syncing", "[driver][rx]") {
    rb_rx_parser p;
    rb_rx_init(&p, 1);
    CHECK_FALSE(rb_rx_feed(&p, rx_word(1, 5)));
    CHECK(p.dropped == 1);
    CHECK_FALSE(rb_rx_feed(&p, rx_word(0, 7)));
    CHECK(rb_rx_feed(&p, rx_word(1, 8)));
    CHECK(p.resyncs == 0);
}

TEST_CASE("RX parser drops a broken frame and resyncs", "[driver][rx]") {
    rb_rx_parser p;
    rb_rx_init(&p, 2);
    CHECK_FALSE(rb_rx_feed(&p, rx_word(0, 1)));
    CHECK_FALSE(rb_rx_feed(&p, rx_word(1, 2)));
    // Index 3 where 2 was expected: two values of the partial frame are lost
    // and the word itself cannot start a frame.
    CHECK_FALSE(rb_rx_feed(&p, rx_word(3, 3)));
    CHECK(p.resyncs == 1);
    CHECK(p.dropped == 3);
    // The offending word may itself be index 0 and restart at once.
    CHECK_FALSE(rb_rx_feed(&p, rx_word(0, 10)));
    CHECK_FALSE(rb_rx_feed(&p, rx_word(0, 11)));
    CHECK(p.resyncs == 2);
    CHECK(p.dropped == 4);
    CHECK_FALSE(rb_rx_feed(&p, rx_word(1, 12)));
    CHECK_FALSE(rb_rx_feed(&p, rx_word(2, 13)));
    CHECK(rb_rx_feed(&p, rx_word(3, 14)));
    CHECK(p.frame[2] == 11);
}

TEST_CASE("RX parser rejects words with the top nibble set", "[driver][rx]") {
    rb_rx_parser p;
    rb_rx_init(&p, 1);
    CHECK_FALSE(rb_rx_feed(&p, 0x10000000u | rx_word(0, 1)));
    CHECK(p.dropped == 1);
    CHECK_FALSE(rb_rx_feed(&p, rx_word(0, 1)));
    CHECK_FALSE(rb_rx_feed(&p, 0xf0000000u | rx_word(1, 2)));
    CHECK(p.resyncs == 1);
    CHECK(p.dropped == 3);
}

TEST_CASE("RX receiver count is clamped and resets the sequence", "[driver][rx]") {
    rb_rx_parser p;
    rb_rx_init(&p, 0);
    CHECK(rb_rx_frame_bytes(&p) == 6);
    rb_rx_set_receivers(&p, 20);
    CHECK(rb_rx_frame_bytes(&p) == RB_MAX_FRAME_BYTES);
    rb_rx_set_receivers(&p, 6);
    CHECK(rb_rx_frame_bytes(&p) == 36);
    CHECK_FALSE(p.synced);
}

TEST_CASE("ring keeps whole puts and wraps", "[driver][ring]") {
    std::array<u8, 8> storage{};
    rb_ring r;
    rb_ring_init(&r, storage.data(), storage.size());
    const u8 a[] = {1, 2, 3, 4, 5, 6};
    CHECK(rb_ring_put(&r, a, 6));
    CHECK(rb_ring_used(&r) == 6);
    CHECK(rb_ring_free(&r) == 2);
    CHECK_FALSE(rb_ring_put(&r, a, 3));  // all or nothing
    CHECK(rb_ring_used(&r) == 6);

    u8 out[8] = {};
    CHECK(rb_ring_get(&r, out, 4) == 4);
    CHECK(out[0] == 1);
    CHECK(out[3] == 4);
    const u8 b[] = {7, 8, 9, 10, 11};
    CHECK(rb_ring_put(&r, b, 5));  // wraps
    CHECK(rb_ring_get(&r, out, 8) == 7);
    const std::vector<u8> expected = {5, 6, 7, 8, 9, 10, 11};
    CHECK(std::vector<u8>(out, out + 7) == expected);
    CHECK(rb_ring_get(&r, out, 8) == 0);

    CHECK(rb_ring_put(&r, a, 6));
    rb_ring_reset(&r);
    CHECK(rb_ring_used(&r) == 0);
}

TEST_CASE("ring counters survive wrap-around of the indices", "[driver][ring]") {
    std::array<u8, 4> storage{};
    rb_ring r;
    rb_ring_init(&r, storage.data(), storage.size());
    r.head = r.tail = 0xfffffffeu;
    const u8 a[] = {1, 2, 3};
    CHECK(rb_ring_put(&r, a, 3));
    CHECK(rb_ring_used(&r) == 3);
    u8 out[3] = {};
    CHECK(rb_ring_get(&r, out, 3) == 3);
    CHECK(out[2] == 3);
}

TEST_CASE("TX packing puts I in the high half, big-endian input", "[driver][tx]") {
    const u8 in[] = {0x12, 0x34, 0x56, 0x78, 0xff, 0xfe, 0x80, 0x00};
    u32 out[2] = {};
    rb_tx_pack(in, out, 2);
    CHECK(out[0] == 0x12345678u);
    CHECK(out[1] == 0xfffe8000u);
}
