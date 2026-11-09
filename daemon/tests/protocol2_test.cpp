// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2/protocol2.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "p2_packets.hpp"
#include "session.hpp"

using namespace radioberry::daemon;
using namespace radioberry::daemon::p2;
using namespace radioberry::daemon::test;

TEST_CASE("the Protocol 2 discovery reply", "[p2]") {
    const Identity id{{2, 0, 0, 0, 0, 9}, 75, 6};
    const ShortPacket idle = discovery_reply(id, false);
    CHECK(idle[0] == 0);
    CHECK(idle[3] == 0);
    CHECK(idle[4] == 0x02);
    CHECK(idle[5] == 2);
    CHECK(idle[10] == 9);
    CHECK(idle[11] == kBoardHermesLite);
    CHECK(idle[12] == kProtocolVersion);
    CHECK(idle[13] == 75);  // 40 or more: a Hermes-Lite 2 to piHPSDR
    CHECK(idle[20] == 6);
    CHECK(discovery_reply(id, true)[4] == 0x03);
    CHECK(is_p2_discovery(p2_discovery_packet()));
    CHECK_FALSE(is_general(p2_discovery_packet()));
}

TEST_CASE("General packets", "[p2]") {
    auto p = general_packet(7, true, true);
    REQUIRE(is_general(p));
    auto g = parse_general(p);
    REQUIRE(g);
    CHECK(g->sequence == 7);
    CHECK(g->pa_enable);
    CHECK(g->phase_words);
    CHECK(g->default_ports);
    CHECK_FALSE(g->wideband);
    p[6] = 0x05;  // a port asked for
    p[23] = 1;
    g = parse_general(p);
    CHECK_FALSE(g->default_ports);
    CHECK(g->wideband);
    CHECK_FALSE(parse_general(general_packet(0, false, false))->phase_words);
    CHECK_FALSE(parse_general(std::vector<std::uint8_t>(59, 0)));
    CHECK(sequence_of(std::vector<std::uint8_t>(2, 0xff)) == 0);
}

TEST_CASE("Receiver Specific packets", "[p2]") {
    const auto r = parse_rx_specific(
        rx_specific_packet(3, {{0, 192, 0, 0}, {1, 48, 1, 0x01}, {kMaxDdcs - 1, 384, 0, 0x02}}));
    REQUIRE(r);
    CHECK(r->sequence == 3);
    CHECK(r->adcs == 1);
    CHECK(r->ddc[0].enabled);
    CHECK(r->ddc[0].rate_ksps == 192);
    CHECK(r->ddc[0].bits == 24);
    CHECK(r->ddc[1].enabled);
    CHECK(r->ddc[1].adc == 1);
    CHECK(r->ddc[1].sync == 0x01);
    CHECK_FALSE(r->ddc[2].enabled);
    CHECK(r->ddc[kMaxDdcs - 1].enabled);
    CHECK(r->ddc[kMaxDdcs - 1].rate_ksps == 384);
    CHECK(r->ddc[kMaxDdcs - 1].sync == 0x02);
    CHECK_FALSE(parse_rx_specific(std::vector<std::uint8_t>(60, 0)));
}

TEST_CASE("Transmitter Specific packets", "[p2]") {
    const auto t = parse_tx_specific(tx_specific_packet(9, 192, 24, 0x02));
    REQUIRE(t);
    CHECK(t->sequence == 9);
    CHECK(t->dacs == 1);
    CHECK(t->cw_mode == 0x02);
    CHECK(t->rate_ksps == 192);
    CHECK(t->bits == 24);
    CHECK_FALSE(parse_tx_specific(std::vector<std::uint8_t>(1444, 0)));
}

TEST_CASE("High Priority packets and phase words", "[p2]") {
    HighPriorityBytes b;
    b.mox = true;
    b.cwx = true;
    b.ddc = {phase_word(7074000), 14074000};
    b.duc = phase_word(7074000);
    b.drive = 0xa0;
    b.attenuation = 31;
    const auto h = parse_high_priority(high_priority_packet(11, b));
    REQUIRE(h);
    CHECK(h->sequence == 11);
    CHECK(h->run);
    CHECK(h->mox);
    CHECK(h->cwx);
    CHECK(frequency_from_phase_word(h->ddc_frequency[0]) == 7074000);
    CHECK(h->ddc_frequency[1] == 14074000);
    CHECK(h->ddc_frequency[2] == 0);
    CHECK(frequency_from_phase_word(h->duc_frequency) == 7074000);
    CHECK(h->drive == 0xa0);
    CHECK(h->attenuation_adc0 == 31);
    CHECK(h->attenuation_adc1 == 0);
    CHECK(frequency_from_phase_word(0) == 0);
    CHECK(frequency_from_phase_word(0x80000000u) == 61440000);
    CHECK_FALSE(parse_high_priority(std::vector<std::uint8_t>(60, 0)));
}

TEST_CASE("TX I/Q packets", "[p2]") {
    const auto p = tx_iq_packet(5, -2, 0x123456);
    const auto t = parse_tx_iq(p);
    REQUIRE(t);
    CHECK(t->sequence == 5);
    REQUIRE(t->samples.size() == kTxSamplesPerPacket * kSampleBytes);
    CHECK(t->samples[0] == 0xff);
    CHECK(t->samples[2] == 0xfe);
    CHECK(t->samples[3] == 0x12);
    CHECK(t->samples[5] == 0x56);
    CHECK_FALSE(parse_tx_iq(std::vector<std::uint8_t>(1443, 0)));
}

TEST_CASE("packets to the host", "[p2]") {
    LongPacket d{};
    d.fill(0xaa);
    begin_ddc_packet(d, 0x01020304);
    CHECK(d[0] == 1);
    CHECK(d[3] == 4);
    for (std::size_t i = 4; i < 12; ++i) {
        CHECK(d[i] == 0);
    }
    CHECK(d[13] == 24);
    CHECK(d[14] == 0);
    CHECK(d[15] == 238);
    CHECK(d[16] == 0xaa);  // samples untouched

    const ShortPacket s = high_priority_status(2, true, true);
    CHECK(s[3] == 2);
    CHECK(s[4] == 0x03);
    CHECK(high_priority_status(0, false, false)[4] == 0);

    const MicPacket m = mic_packet(0x10000);
    CHECK(m[1] == 1);
    CHECK(m[4] == 0);
    CHECK(m[131] == 0);
}
