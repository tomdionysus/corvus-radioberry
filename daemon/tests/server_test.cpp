// SPDX-License-Identifier: GPL-2.0-or-later
#include "p1/server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "frequency.hpp"
#include "p1_packets.hpp"

using namespace radioberry;
using namespace radioberry::daemon;
using namespace radioberry::daemon::p1;
using namespace radioberry::daemon::test;
using namespace std::chrono_literals;

namespace {

struct FakeLink final : Link {
    struct Sent {
        Peer to;
        std::vector<std::uint8_t> bytes;
    };
    void send(const Peer& to, std::span<const std::uint8_t> b) override {
        sent.push_back({to, {b.begin(), b.end()}});
    }
    void close(const Peer& peer) override { closed.push_back(peer); }
    std::vector<Sent> sent;
    std::vector<Peer> closed;
};

const Peer kA{0x0a000001, 50000, false};
const Peer kB{0x0a000002, 50001, false};
const Peer kTcp{0x0a000003, 50002, true};

struct Rig {
    explicit Rig(TxPolicyOptions o = {}, std::uint32_t rx_values_max = 0)
        : values_max(rx_values_max), policy(control, session, o) {}
    std::uint32_t values_max;  // the driver's RX limit; 0 the Pi 5's
    Control control;
    SpscRing<TxBlock> tx{2};
    FakeLink link;
    GatewareStatus status;
    Session session{[this] { control.safe(); }, 3s};
    TxPolicy policy;
    Server server{Identity{{2, 0, 0, 0, 0, 1}, 75, 3, 6, values_max},
                  control,
                  tx,
                  session,
                  policy,
                  [this] { return status; },
                  link};

    // As the daemon's loop does.
    void tick(std::chrono::nanoseconds now) {
        session.tick(now);
        server.tick();
    }

    void packet(const std::vector<std::uint8_t>& p, const Peer& from,
                std::chrono::nanoseconds now = 0s) {
        server.on_packet(p, from, now);
    }
    // Frames for one whole packet at the client's receiver count.
    void rx_packet(unsigned receivers = 1) {
        RxBlock b;
        b.receivers = receivers;
        b.size = 2 * samples_per_half(server.client_receivers()) * 6u * receivers;
        server.on_rx(b);
    }
    std::vector<Command> drain() {
        std::vector<Command> out;
        while (const auto c = control.next()) {
            out.push_back(*c);
        }
        return out;
    }
};

}  // namespace

TEST_CASE("discovery is answered to the sender", "[server]") {
    Rig rig;
    rig.packet(discovery_packet(), kA);
    REQUIRE(rig.link.sent.size() == 1);
    CHECK(rig.link.sent[0].to == kA);
    CHECK(rig.link.sent[0].bytes.size() == 60);
    CHECK(rig.link.sent[0].bytes[2] == 0x02);
    CHECK(rig.link.sent[0].bytes[19] == 6);
    CHECK(rig.link.closed.empty());

    // Over TCP the connection is closed after the reply.
    rig.packet(discovery_packet(1032), kTcp);
    REQUIRE(rig.link.closed.size() == 1);
    CHECK(rig.link.closed[0] == kTcp);

    // While streaming, discovery says so.
    rig.packet(start_packet(1), kA);
    rig.packet(discovery_packet(), kB);
    CHECK(rig.link.sent.back().bytes[2] == 0x03);
}

TEST_CASE("a started client receives packets until it stops", "[server]") {
    Rig rig;
    rig.rx_packet();  // not running: nothing sent
    CHECK(rig.link.sent.empty());

    rig.packet(start_packet(1), kA);
    CHECK(rig.server.running());
    REQUIRE(rig.server.client());
    CHECK(*rig.server.client() == kA);
    // Starting sets the PTT hang time and the TX buffer latency.
    const auto timing = rig.drain();
    REQUIRE(timing.size() == 1);
    CHECK(timing[0].address == 0x17);
    CHECK(timing[0].data == (12u << 8 | 40));
    rig.status = {true, true};
    rig.rx_packet();
    rig.rx_packet();
    REQUIRE(rig.link.sent.size() == 2);
    const auto& p = rig.link.sent[0].bytes;
    CHECK(rig.link.sent[0].to == kA);
    CHECK(p.size() == 1032);
    CHECK(p[3] == 0x06);
    CHECK(p[11] == 0x05);  // status address 0 with PTT and CW key
    CHECK(p[12] == 0x1e);
    CHECK(p[15] == 75);
    CHECK(p[8 + 512 + 3] == 0x0d);  // address 1
    CHECK(rig.link.sent[1].bytes[7] == 1);
    CHECK(rig.link.sent[1].bytes[11] == 0x15);  // address 2
    CHECK(rig.server.counters().packets_out == 2);

    rig.packet(start_packet(0), kA);
    CHECK_FALSE(rig.server.running());
    CHECK_FALSE(rig.server.client());
    rig.rx_packet();
    CHECK(rig.link.sent.size() == 2);
    // Stopping puts the gateware in the safe state.
    const auto commands = rig.drain();
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].address == 0x09);
    CHECK_FALSE(commands[0].can_transmit());
    // A stop when not running changes nothing.
    rig.packet(start_packet(0), kA);
    CHECK(rig.drain().empty());
}

TEST_CASE("a second client is ignored until the first stops or times out", "[server]") {
    Rig rig;
    rig.packet(start_packet(1), kA, 0s);
    rig.packet(start_packet(1), kB, 1s);
    rig.packet(host_packet(0, {0x04, 7074000}, {}), kB, 1s);
    rig.packet(start_packet(0), kB, 1s);
    CHECK(*rig.server.client() == kA);
    CHECK(rig.server.running());
    CHECK_FALSE(rig.control.value(0x02));
    CHECK(rig.server.counters().ignored == 3);

    // The owner's packets keep it alive; silence for the timeout ends it.
    rig.packet(host_packet(0, {}, {}), kA, 2s);
    rig.tick(4s);
    CHECK(rig.server.running());
    rig.tick(5s);
    CHECK_FALSE(rig.server.running());
    CHECK(rig.link.closed.empty());  // UDP: nothing to close

    rig.packet(start_packet(1), kB, 6s);
    CHECK(*rig.server.client() == kB);
    rig.tick(6s);
    CHECK(rig.server.running());
}

TEST_CASE("a TCP client's connection is closed when it stops or times out", "[server]") {
    Rig rig;
    rig.packet(start_packet(1, 1032), kTcp, 0s);
    rig.packet(discovery_packet(1032), kTcp, 0s);  // on the streaming connection: kept
    CHECK(rig.link.closed.empty());
    rig.packet(start_packet(0, 1032), kTcp, 1s);
    REQUIRE(rig.link.closed.size() == 1);
    rig.packet(start_packet(1, 1032), kTcp, 2s);
    rig.tick(10s);
    CHECK(rig.link.closed.size() == 2);
    CHECK_FALSE(rig.server.running());
}

TEST_CASE("commands go to the gateware when they change", "[server]") {
    Rig rig;
    // Commands are taken before start, as clients send them.
    rig.packet(host_packet(0, {0x04, 14074000}, {0x02, 0x55}), kA);
    auto commands = rig.drain();
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].address == 0x02);
    CHECK(commands[0].data == 14074000);
    CHECK(commands[1].address == 0x01);
    rig.packet(host_packet(1, {0x04, 14074000}, {0x02, 0x55}), kA);
    CHECK(rig.drain().empty());
    rig.packet(host_packet(2, {0x04, 14075000}, {0x02, 0x55}), kA);
    CHECK(rig.drain().size() == 1);
}

TEST_CASE("receiver counts beyond the device are zero-filled", "[server]") {
    Rig rig;
    // Four receivers at 384 ksps: the bus carries three.
    rig.packet(host_packet(0, {0x00, 3u << 24 | 3u << 3 | 1u << 2}, {0x04, 7074000}), kA);
    CHECK(rig.server.client_receivers() == 4);
    CHECK(rig.server.device_receivers() == 3);
    auto commands = rig.drain();
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].data == (3u << 24 | 2u << 3 | 1u << 2));
    // Then two at 48 ksps.
    rig.packet(host_packet(1, {0x00, 1u << 3}, {0x04, 7074000}), kA);
    CHECK(rig.server.client_receivers() == 2);
    CHECK(rig.server.device_receivers() == 2);
    // The same request again changes nothing.
    rig.packet(host_packet(2, {0x00, 1u << 3}, {0x04, 7074000}), kA);
    commands = rig.drain();
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].data == 1u << 3);

    rig.packet(start_packet(1), kA);
    rig.rx_packet(2);
    REQUIRE(rig.link.sent.size() == 1);
}

TEST_CASE("a slower bus, as on a Pi 4, runs fewer receivers", "[server]") {
    Rig rig({}, 192000);
    // Three receivers at 48 ksps: 192 000 values a second carry two.
    rig.packet(host_packet(0, {0x00, 2u << 3 | 1u << 2}, {0x04, 7074000}), kA);
    CHECK(rig.server.client_receivers() == 3);
    CHECK(rig.server.device_receivers() == 2);
    auto commands = rig.drain();
    REQUIRE_FALSE(commands.empty());
    CHECK(commands[0].data == (1u << 3 | 1u << 2));
    // One at 192 ksps is more than it carries, but one always runs.
    rig.packet(host_packet(1, {0x00, 2u << 24}, {0x04, 7074000}), kA);
    CHECK(rig.server.device_receivers() == 1);
    commands = rig.drain();
    REQUIRE_FALSE(commands.empty());
    CHECK(commands[0].data == 2u << 24);
}

TEST_CASE("without --allow-tx nothing transmits", "[server]") {
    Rig rig;
    rig.packet(start_packet(1), kA);
    REQUIRE(rig.drain().size() == 1);  // the TX timing set at start
    // MOX, drive 255 with PA enable and VNA, and a disabled TX watchdog.
    rig.packet(
        host_packet(0, {0x13, 0xffu << 24 | 1u << 23 | 1u << 19 | 1u << 18}, {0x2e, 31u << 8 | 20}),
        kA);
    rig.packet(
        host_packet(1, {0x13, 0xffu << 24 | 1u << 23 | 1u << 19 | 1u << 18}, {0x2e, 31u << 8 | 20}),
        kA);
    CHECK(rig.control.transmit() == TransmitState{});
    const auto commands = rig.drain();
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].address == 0x09);
    CHECK(commands[0].data == 1u << 18);
    CHECK_FALSE(commands[0].ptt);
    CHECK(rig.control.value(0x17) == (12u << 8 | 40));  // the start value stands
    // A valid hang time afterwards is taken, its latency raised to 40 ms.
    rig.packet(host_packet(2, {0x2e, 10u << 8 | 20}, {}), kA);
    CHECK(rig.control.value(0x17) == (10u << 8 | 40));
    // A latency of 40 ms or more is taken as sent.
    rig.packet(host_packet(3, {0x2e, 10u << 8 | 60}, {}), kA);
    CHECK(rig.control.value(0x17) == (10u << 8 | 60));
    // MOX cleared, then set again: warned again (not observable here, but
    // the path runs).
    rig.packet(host_packet(4, {}, {}), kA);
    rig.packet(host_packet(5, {0x01, 0}, {}), kA);
    CHECK(rig.control.transmit() == TransmitState{});
}

TEST_CASE("a command with C0 bit 7 is echoed in the next response", "[server]") {
    Rig rig;
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(0, {0x80 | 0x3c << 1, 0x06aa0000}, {}), kA);
    rig.rx_packet();
    const auto& p = rig.link.sent[0].bytes;
    CHECK(p[11] == 0xf8);
    CHECK(p[12] == 0x06);
    CHECK(p[13] == 0xaa);
    CHECK(p[8 + 512 + 3] == 0x00);  // then status address 0 resumes
}

TEST_CASE("the client's TX samples go to the TX ring while streaming", "[server]") {
    Rig rig;
    rig.packet(host_packet(0, {}, {}, 0x1234, 0x5678), kA);
    CHECK(rig.tx.empty());  // not streaming
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(1, {}, {}, 0x1234, 0x5678), kA);
    REQUIRE(rig.tx.front() != nullptr);
    CHECK(rig.tx.front()->size == 504);
    CHECK(rig.tx.front()->bytes[0] == 0x12);
    CHECK(rig.tx.front()->bytes[503] == 0x78);
    rig.packet(host_packet(2, {}, {}), kA);
    rig.packet(host_packet(3, {}, {}), kA);  // the ring holds two
    CHECK(rig.server.counters().tx_blocks_dropped == 1);
    CHECK(rig.session.faults() == 1);
}

TEST_CASE("host sequence gaps and junk are counted", "[server]") {
    Rig rig;
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(5, {}, {}), kA);
    rig.packet(host_packet(6, {}, {}), kA);
    rig.packet(host_packet(9, {}, {}), kA);
    rig.packet(host_packet(0, {}, {}), kA);  // a restart is no gap
    CHECK(rig.server.counters().host_sequence_gaps == 1);
    // The gap and two TX-queue-full (the ring holds two): counted, and with
    // no MOX nothing is latched.
    CHECK(rig.session.faults() == 3);
    CHECK_FALSE(rig.session.latched());
    const std::vector<std::uint8_t> junk = {1, 2, 3, 4};
    rig.packet(junk, kA);
    auto bad = host_packet(1, {}, {});
    bad.resize(1000);
    rig.packet(bad, kA);
    CHECK(rig.server.counters().ignored == 2);
    CHECK(rig.server.counters().packets_in == 7);
}

TEST_CASE("a client that lost the radio to a timeout stops being served", "[server]") {
    Rig rig;
    rig.packet(start_packet(1), kA, 0s);
    REQUIRE(rig.session.owns({Protocol::P1, kA}));
    rig.session.tick(10s);  // the session expires the owner
    CHECK(rig.server.running());
    rig.server.tick();
    CHECK_FALSE(rig.server.running());
    rig.server.tick();  // nothing more to do
    CHECK_FALSE(rig.server.running());
    // Its late packets no longer feed the TX ring.
    rig.packet(host_packet(1, {}, {}), kA, 11s);
    CHECK(rig.tx.empty());
}

namespace {

TxPolicyOptions transmit_allowed() {
    TxPolicyOptions o;
    o.allow_tx = true;
    o.allow_pa = true;
    o.drive_ceiling = 0x40;
    return o;
}

}  // namespace

TEST_CASE("the owner's MOX keys the transmitter through the policy", "[server]") {
    Rig rig(transmit_allowed());
    rig.packet(start_packet(1), kA);
    // Drive 255 with PA enable, then MOX in the second block.
    rig.packet(host_packet(0, {0x13, 0xffu << 24 | 1u << 19}, {0x01, 14074000}, 0x1000, 0), kA);
    CHECK(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{true, true, false});
    CHECK(rig.control.value(0x09) == (0x40u << 24 | 1u << 19));
    REQUIRE(rig.tx.front() != nullptr);
    CHECK(rig.tx.front()->bytes[0] == 0x10);
    // MOX released: PTT, TX permission and PA enable clear; drive kept.
    rig.packet(host_packet(1, {0x13, 0xffu << 24 | 1u << 19}, {0x00, 0}), kA);
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
    CHECK(rig.control.value(0x09) == 0x40u << 24);
    for (const Command& c : rig.drain()) {
        if (c.address == 0x09 && (c.data & (1u << 19)) != 0) {
            CHECK(c.ptt);  // PA enable only ever goes out with PTT
        }
    }
}

TEST_CASE("MOX in the first block alone does not key", "[server]") {
    Rig rig(transmit_allowed());
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(0, {0x01, 0}, {0x04, 7074000}), kA);
    CHECK_FALSE(rig.policy.transmitting());
}

TEST_CASE("MOX from a client that is not streaming does not key", "[server]") {
    Rig rig(transmit_allowed());
    rig.packet(host_packet(0, {0x01, 0}, {0x03, 14074000}), kA);  // before start
    CHECK_FALSE(rig.policy.transmitting());
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(0, {0x01, 0}, {0x03, 14074000}), kB);  // not the owner
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
}

TEST_CASE("CWX from address 0x0f goes through the policy", "[server]") {
    Rig rig(transmit_allowed());
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(0, {0x1e, 1u << 24}, {0x00, 0}), kA);  // CWX, no MOX
    CHECK_FALSE(rig.control.transmit().cwx);
    CHECK(rig.control.value(0x0f) == 1u << 24);  // the rest of 0x0f is forwarded
    rig.packet(host_packet(1, {0x1e, 1u << 24}, {0x01, 0}), kA);
    CHECK(rig.control.transmit().cwx);
}

TEST_CASE("a host sequence gap while transmitting drops MOX until released", "[server]") {
    Rig rig(transmit_allowed());
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(0, {0x02, 0}, {0x01, 0}), kA);
    CHECK(rig.policy.transmitting());
    rig.tx.pop();
    rig.packet(host_packet(5, {0x02, 0}, {0x01, 0}), kA);  // packets 1 to 4 lost
    CHECK(rig.session.latched());
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
    rig.tx.pop();
    rig.packet(host_packet(6, {0x02, 0}, {0x01, 0}), kA);
    CHECK_FALSE(rig.policy.transmitting());
    rig.tx.pop();
    rig.packet(host_packet(7, {0x02, 0}, {0x00, 0}), kA);  // released
    rig.tx.pop();
    rig.packet(host_packet(8, {0x02, 0}, {0x01, 0}), kA);
    CHECK(rig.policy.transmitting());
}

TEST_CASE("stopping while transmitting leaves the gateware safe", "[server]") {
    Rig rig(transmit_allowed());
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(0, {0x13, 0x20u << 24 | 1u << 19}, {0x01, 0}), kA);
    REQUIRE(rig.policy.transmitting());
    rig.packet(start_packet(0), kA);
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
    CHECK((*rig.control.value(0x09) & (1u << 19)) == 0);
}

TEST_CASE("frequency commands are corrected for the radio's error", "[server]") {
    CHECK(is_frequency_address(0x01));
    CHECK(is_frequency_address(0x02));
    CHECK(is_frequency_address(0x08));
    CHECK(is_frequency_address(0x12));
    CHECK(is_frequency_address(0x16));
    CHECK_FALSE(is_frequency_address(0x00));
    CHECK_FALSE(is_frequency_address(0x09));
    CHECK_FALSE(is_frequency_address(0x11));
    CHECK_FALSE(is_frequency_address(0x17));
    CHECK(corrected_frequency(7074000, 0) == 7074000);
    // 22.9 ppm high: 162 Hz less is commanded at 7.074 MHz.
    CHECK(corrected_frequency(7074000, 22.9) == 7073838);
    CHECK(corrected_frequency(14074000, -10) == 14074141);
    CHECK(corrected_frequency(0xffffffffu, -200) == 0xffffffffu);
    CHECK(corrected_frequency(1, 1e9) == 0);

    Rig rig;
    rig.server.set_frequency_ppm(22.9);
    rig.packet(host_packet(0, {0x04, 7074000}, {0x02, 7074000}), kA);
    rig.packet(host_packet(1, {0x24, 7074000}, {0x14, 1000}),
               kA);  // RX8, and 0x0a: not a frequency
    CHECK(rig.control.value(0x02) == 7073838u);
    CHECK(rig.control.value(0x01) == 7073838u);
    CHECK(rig.control.value(0x12) == 7073838u);
    CHECK(rig.control.value(0x0a) == 1000u);
    // The same frequency again is not resent.
    rig.drain();
    rig.packet(host_packet(2, {0x04, 7074000}, {0x02, 7074000}), kA);
    CHECK(rig.drain().empty());
}

TEST_CASE("the standard convention swaps TX I and Q", "[server]") {
    Rig rig;
    rig.server.set_iq_convention(IqConvention::Standard);
    rig.packet(start_packet(1), kA);
    rig.packet(host_packet(0, {}, {}, 0x1234, 0x5678), kA);
    REQUIRE(rig.tx.front() != nullptr);
    CHECK(rig.tx.front()->bytes[0] == 0x56);
    CHECK(rig.tx.front()->bytes[1] == 0x78);
    CHECK(rig.tx.front()->bytes[2] == 0x12);
    CHECK(rig.tx.front()->bytes[3] == 0x34);
    rig.server.set_iq_convention(IqConvention::Hl2);
    rig.tx.pop();
    rig.packet(host_packet(1, {}, {}, 0x1234, 0x5678), kA);
    CHECK(rig.tx.front()->bytes[0] == 0x12);
}
