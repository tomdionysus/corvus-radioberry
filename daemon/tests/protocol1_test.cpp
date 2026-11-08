// SPDX-License-Identifier: GPL-2.0-or-later
#include "p1/protocol1.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <vector>

#include "device/capacity.hpp"
#include "p1_packets.hpp"

using namespace radioberry::daemon;
using namespace radioberry::daemon::p1;
using namespace radioberry::daemon::test;

namespace {

RxBlock block(unsigned receivers, std::size_t frames, std::uint8_t first = 0) {
    RxBlock b;
    b.receivers = receivers;
    b.size = frames * 6u * receivers;
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t r = 0; r < 6u * receivers; ++r) {
            b.bytes[f * 6u * receivers + r] =
                static_cast<std::uint8_t>(first + f + 0x40 * (r / 6) % 256);
        }
    }
    return b;
}

}  // namespace

TEST_CASE("the RX capacity limits receivers by rate", "[p1]") {
    CHECK(rx_rate_hz(0) == 48000);
    CHECK(rx_rate_hz(3) == 384000);
    CHECK(max_receivers(0, 6) == 6);
    CHECK(max_receivers(1, 12) == 12);
    CHECK(max_receivers(2, 6) == 6);
    CHECK(max_receivers(3, 6) == 3);
    CHECK(max_receivers(3, 2) == 2);
}

TEST_CASE("packets are classified by their first four bytes", "[p1]") {
    CHECK(classify(discovery_packet()) == PacketKind::Discovery);
    CHECK(classify(start_packet(1)) == PacketKind::StartStop);
    CHECK(classify(host_packet(0, {}, {})) == PacketKind::Data);
    const std::vector<std::uint8_t> short_packet = {0xef, 0xfe, 0x02};
    CHECK(classify(short_packet) == PacketKind::Unknown);
    const std::vector<std::uint8_t> other = {0xef, 0xfe, 0x01, 0x04};
    CHECK(classify(other) == PacketKind::Unknown);
    const std::vector<std::uint8_t> p2 = {0xef, 0xfe, 0x05, 0x00};
    CHECK(classify(p2) == PacketKind::Unknown);
    const std::vector<std::uint8_t> junk = {0x00, 0xfe, 0x02, 0x00};
    CHECK(classify(junk) == PacketKind::Unknown);
}

TEST_CASE("the discovery reply is the HL2's", "[p1]") {
    Identity id;
    id.mac = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    id.version_major = 75;
    id.version_minor = 3;
    id.receivers = 6;
    const auto r = discovery_reply(id, false);
    CHECK(r.size() == 60);
    CHECK(r[0] == 0xef);
    CHECK(r[1] == 0xfe);
    CHECK(r[2] == 0x02);
    CHECK(r[3] == 0x02);
    CHECK(r[8] == 0x55);
    CHECK(r[9] == 75);
    CHECK(r[10] == 0x06);
    CHECK(r[11] == 0);
    CHECK(r[19] == 6);
    CHECK(r[20] == 0);
    CHECK(r[21] == 3);
    CHECK(discovery_reply(id, true)[2] == 0x03);
}

TEST_CASE("start and stop", "[p1]") {
    CHECK(parse_start(start_packet(0x01)) == true);
    CHECK(parse_start(start_packet(0x03)) == true);
    CHECK(parse_start(start_packet(0x11)) == true);
    CHECK(parse_start(start_packet(0x00)) == false);
    CHECK(parse_start(start_packet(0x02)) == false);
    CHECK_FALSE(parse_start(discovery_packet()));
}

TEST_CASE("a host packet yields its commands and TX samples", "[p1]") {
    const auto p = host_packet(0x01020304, {0x01, 0x01000008}, {0x84, 14074000}, 0x1234, 0xabcd);
    const auto h = parse_host_packet(p);
    REQUIRE(h);
    CHECK(h->sequence == 0x01020304);
    CHECK(h->commands[0].address == 0x00);
    CHECK(h->commands[0].mox);
    CHECK_FALSE(h->commands[0].ack);
    CHECK(h->commands[0].data == 0x01000008);
    CHECK(h->commands[1].address == 0x02);
    CHECK(h->commands[1].ack);
    CHECK_FALSE(h->commands[1].mox);
    CHECK(h->commands[1].data == 14074000);
    for (std::size_t s = 0; s < kTxSamplesPerPacket; ++s) {
        INFO(s);
        REQUIRE(h->tx_iq[4 * s] == 0x12);
        REQUIRE(h->tx_iq[4 * s + 1] == 0x34);
        REQUIRE(h->tx_iq[4 * s + 2] == 0xab);
        REQUIRE(h->tx_iq[4 * s + 3] == 0xcd);
    }

    auto truncated = p;
    truncated.pop_back();
    CHECK_FALSE(parse_host_packet(truncated));
    auto unsynced = p;
    unsynced[8 + 512] = 0;
    CHECK_FALSE(parse_host_packet(unsynced));
    unsynced = p;
    unsynced[9] = 0;
    CHECK_FALSE(parse_host_packet(unsynced));
    CHECK_FALSE(parse_host_packet(start_packet(1, 1032)));
}

TEST_CASE("response words", "[p1]") {
    CHECK(status_word(0, false, false, 75) == Word{0x00, 0x1e, 0x00, 0x00, 75});
    CHECK(status_word(0, true, true, 75) == Word{0x05, 0x1e, 0x00, 0x00, 75});
    CHECK(status_word(1, false, false, 75) == Word{0x08, 0, 0, 0, 0});
    CHECK(status_word(2, true, false, 75) == Word{0x11, 0, 0, 0, 0});
    CHECK(status_word(3, false, true, 75) == Word{0x1c, 0, 0, 0, 0});
    CHECK(status_word(4, false, false, 75) == status_word(0, false, false, 75));
    CHECK(ack_word(0x3c, false, 0x01020304) == Word{0xf8, 0x01, 0x02, 0x03, 0x04});
    CHECK(ack_word(0x02, true, 0) == Word{0x85, 0, 0, 0, 0});
}

TEST_CASE("samples per half", "[p1]") {
    CHECK(samples_per_half(1) == 63);
    CHECK(samples_per_half(2) == 36);
    CHECK(samples_per_half(4) == 19);
    CHECK(samples_per_half(6) == 13);
    CHECK(samples_per_half(12) == 6);
}

TEST_CASE("the packer fills two halves per packet", "[p1]") {
    RxPacker packer;
    std::vector<std::vector<std::uint8_t>> packets;
    int words = 0;
    const auto word = [&] { return Word{static_cast<std::uint8_t>(++words), 2, 3, 4, 5}; };
    const auto emit = [&](std::span<const std::uint8_t> p) {
        packets.emplace_back(p.begin(), p.end());
    };

    packer.add(block(1, 125), word, emit);
    CHECK(packets.empty());
    packer.add(block(1, 2, 125), word, emit);
    REQUIRE(packets.size() == 1);
    CHECK(packer.sequence() == 1);
    const auto& p = packets[0];
    CHECK(p.size() == 1032);
    CHECK(p[0] == 0xef);
    CHECK(p[3] == 0x06);
    CHECK(p[7] == 0);  // sequence 0
    for (std::size_t half = 0; half < 2; ++half) {
        const std::uint8_t* h = p.data() + 8 + 512 * half;
        CHECK(h[0] == 0x7f);
        CHECK(h[2] == 0x7f);
        CHECK(h[3] == half + 1);  // one word per half, in order
        CHECK(h[7] == 5);
        // Sample 0 of the half: frame 63 * half.
        CHECK(h[8] == 63 * half);
        CHECK(h[13] == 63 * half);
        CHECK(h[14] == 0);  // microphone
        CHECK(h[15] == 0);
        CHECK(h[8 + 8 * 62] == 63 * half + 62);
        CHECK(h[511] == 0);  // no padding needed at one receiver: 63 * 8 = 504
    }

    // The next packet carries the 127th frame and sequence 1.
    packer.add(block(1, 125, 0), word, emit);
    REQUIRE(packets.size() == 2);
    CHECK(packets[1][7] == 1);
    CHECK(packets[1][16] == 126);
}

TEST_CASE("the packer zero-fills receivers the device does not run", "[p1]") {
    RxPacker packer;
    packer.set_receivers(3);
    CHECK(packer.receivers() == 3);
    std::vector<std::vector<std::uint8_t>> packets;
    const auto word = [] { return Word{}; };
    const auto emit = [&](std::span<const std::uint8_t> p) {
        packets.emplace_back(p.begin(), p.end());
    };
    const std::size_t per_packet = 2 * samples_per_half(3);  // 2 * 25
    packer.add(block(2, per_packet, 1), word, emit);
    REQUIRE(packets.size() == 1);
    const std::uint8_t* s = packets[0].data() + 16;
    CHECK(s[0] == 1);     // receiver 0
    CHECK(s[6] == 0x41);  // receiver 1
    CHECK(s[12] == 0);    // receiver 2: absent
    CHECK(s[17] == 0);
    CHECK(s[20] == 2);  // next sample, receiver 0, after 20 bytes
    // 25 samples of 20 bytes leave 4 bytes of padding.
    CHECK(packets[0][8 + 8 + 500] == 0);
}

TEST_CASE("the packer leaves out receivers the client did not ask for", "[p1]") {
    RxPacker packer;
    packer.set_receivers(1);
    std::vector<std::vector<std::uint8_t>> packets;
    const auto word = [] { return Word{}; };
    const auto emit = [&](std::span<const std::uint8_t> p) {
        packets.emplace_back(p.begin(), p.end());
    };
    packer.add(block(3, 126), word, emit);
    REQUIRE(packets.size() == 1);
    CHECK(packets[0][16] == 0);
    CHECK(packets[0][16 + 6] == 0);  // microphone, not receiver 1
    CHECK(packets[0][16 + 8] == 1);
}

TEST_CASE("a receiver-count change or reset drops the packet in progress", "[p1]") {
    RxPacker packer;
    std::vector<std::vector<std::uint8_t>> packets;
    const auto word = [] { return Word{}; };
    const auto emit = [&](std::span<const std::uint8_t> p) {
        packets.emplace_back(p.begin(), p.end());
    };
    packer.add(block(1, 100), word, emit);
    packer.set_receivers(1);  // unchanged: kept
    packer.add(block(1, 26), word, emit);
    CHECK(packets.size() == 1);
    packer.add(block(1, 100), word, emit);
    packer.set_receivers(2);
    packer.add(block(2, 71), word, emit);
    CHECK(packets.size() == 1);
    packer.add(block(2, 1), word, emit);
    CHECK(packets.size() == 2);
    CHECK(packets[1][7] == 1);
    packer.add(block(2, 10), word, emit);
    packer.reset();
    packer.add(block(2, 72), word, emit);
    REQUIRE(packets.size() == 3);
    CHECK(packets[2][7] == 0);

    CHECK_THROWS_AS(packer.set_receivers(0), std::out_of_range);
    CHECK_THROWS_AS(packer.set_receivers(17), std::out_of_range);
}
