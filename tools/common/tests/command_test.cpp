// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/command.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>
#include <vector>

using namespace radioberry;

TEST_CASE("commands encode as gateware bytes", "[command]") {
    Command c;
    c.address = 0x09;
    c.data = 0x01020304;
    CHECK(c.encode() == std::array<std::uint8_t, 6>{0x01, 0x12, 0x01, 0x02, 0x03, 0x04});
    c.ptt = true;
    c.flags = Command::kRun | Command::kTxPermit;
    CHECK(c.encode()[0] == 0x05);
    CHECK(c.encode()[1] == 0x13);
    // Addresses above six bits never reach the PTT bit or beyond.
    c.ptt = false;
    c.address = 0xff;
    CHECK(c.encode()[1] == 0x7e);
}

TEST_CASE("commands decode from bytes", "[command]") {
    const std::array<std::uint8_t, 6> bytes = {0x07, 0x15, 0xaa, 0xbb, 0xcc, 0xdd};
    const Command c = Command::decode(bytes);
    CHECK(c.flags == 0x07);
    CHECK(c.address == 0x0a);
    CHECK(c.ptt);
    CHECK(c.data == 0xaabbccddu);
    CHECK(Command::decode(c.encode()) == c);
}

TEST_CASE("which commands can transmit", "[command]") {
    Command c;
    CHECK_FALSE(c.can_transmit());
    c.ptt = true;
    CHECK(c.can_transmit());
    c.ptt = false;
    c.flags = Command::kRun | Command::kTxPermit;
    CHECK(c.can_transmit());
    c.flags = Command::kCwx;
    CHECK(c.can_transmit());
    c.flags = 0;
    CHECK_FALSE(c.can_transmit());
}

TEST_CASE("replies decode the identity word", "[command]") {
    // CL025: one transmitter, six receivers, type 2, version 75.3; CW key.
    const std::array<std::uint8_t, 6> bytes = {0x05, 0, 0, 0x40 | 6 << 2 | 2, 75, 3};
    const Reply r = Reply::decode(bytes);
    CHECK(r.external_cw_key);
    CHECK(r.cw_or_ptt);
    CHECK(r.transmitters == 1);
    CHECK(r.receivers == 6);
    CHECK(r.fpga_type == 2);
    CHECK(r.version_major == 75);
    CHECK(r.version_minor == 3);
    const Reply idle = Reply::decode(std::array<std::uint8_t, 6>{});
    CHECK_FALSE(idle.external_cw_key);
    CHECK_FALSE(idle.cw_or_ptt);
}

TEST_CASE("rate codes", "[command]") {
    CHECK(rate_code_for_ksps(48) == 0u);
    CHECK(rate_code_for_ksps(96) == 1u);
    CHECK(rate_code_for_ksps(192) == 2u);
    CHECK(rate_code_for_ksps(384) == 3u);
    CHECK_FALSE(rate_code_for_ksps(44).has_value());
}

TEST_CASE("receiver frequency addresses", "[command]") {
    CHECK(rx_frequency_address(0) == 0x02);
    CHECK(rx_frequency_address(5) == 0x07);
    CHECK(rx_frequency_address(6) == 0x08);
    CHECK(rx_frequency_address(7) == 0x12);
    CHECK(rx_frequency_address(11) == 0x16);
    CHECK_THROWS_AS(rx_frequency_address(12), std::out_of_range);
}

TEST_CASE("Hermes-Lite 2 commands", "[command]") {
    const Command g = hl2::general(2, 6);
    CHECK(g.address == 0x00);
    CHECK(g.data == (2u << 24 | 5u << 3 | 1u << 2));
    CHECK(g.flags == Command::kRun);
    CHECK_FALSE(g.can_transmit());
    CHECK(hl2::general(0, 1).data == 1u << 2);
    CHECK_THROWS_AS(hl2::general(4, 1), std::out_of_range);
    CHECK_THROWS_AS(hl2::general(0, 0), std::out_of_range);
    CHECK_THROWS_AS(hl2::general(0, 13), std::out_of_range);

    const Command f = hl2::rx_frequency(1, 7074000);
    CHECK(f.address == 0x03);
    CHECK(f.data == 7074000u);
    CHECK_FALSE(f.can_transmit());

    CHECK(hl2::lna_gain(20).address == 0x0a);
    CHECK(hl2::lna_gain(20).data == (0x40u | 32u));
    CHECK(hl2::lna_gain(-12).data == 0x40u);
    CHECK(hl2::lna_gain(48).data == (0x40u | 60u));
    CHECK_FALSE(hl2::lna_gain(0).can_transmit());
    CHECK_THROWS_AS(hl2::lna_gain(-13), std::out_of_range);
    CHECK_THROWS_AS(hl2::lna_gain(49), std::out_of_range);
}

TEST_CASE("identity text", "[command]") {
    CHECK(version_text(75, 3) == "75.3");
    CHECK(fpga_text(1) == "10CL016");
    CHECK(fpga_text(2) == "10CL025");
    CHECK(fpga_text(0) == "unknown (0)");
    CHECK(driver_version_text(0x010203) == "1.2.3");
}

TEST_CASE("transmit hazards", "[command]") {
    using V = std::vector<std::string>;
    CHECK(transmit_hazards(Command{}).empty());
    CHECK(transmit_hazards(hl2::general(3, 6)).empty());
    Command c;
    c.ptt = true;
    c.flags = Command::kRun | Command::kTxPermit | Command::kCwx;
    CHECK(transmit_hazards(c) == V{"PTT", "TX permission", "CWX"});

    Command pa;
    pa.address = 0x09;
    CHECK(transmit_hazards(pa).empty());
    pa.data = 1u << 18;  // T/R relay disable alone is harmless
    CHECK(transmit_hazards(pa).empty());
    pa.data = 0xff000000u | 1u << 23 | 1u << 19;
    CHECK(transmit_hazards(pa) == V{"drive", "VNA", "PA enable"});
    // The same bits at another address mean something else.
    pa.address = 0x0a;
    CHECK(transmit_hazards(pa).empty());
}

TEST_CASE("PTT hang time 31 disables the TX watchdog", "[command]") {
    Command c;
    c.address = 0x17;
    c.data = 12u << 8 | 20u;
    CHECK_FALSE(disables_tx_watchdog(c));
    c.data = 31u << 8 | 20u;
    CHECK(disables_tx_watchdog(c));
    c.address = 0x16;
    CHECK_FALSE(disables_tx_watchdog(c));
}
