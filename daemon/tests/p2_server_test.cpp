// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <vector>

#include "frequency.hpp"
#include "p1_packets.hpp"
#include "p2/server.hpp"
#include "p2_packets.hpp"

using namespace radioberry;
using namespace radioberry::daemon;
using namespace radioberry::daemon::p2;
using namespace radioberry::daemon::test;
using namespace std::chrono_literals;

namespace {

struct FakeLink final : Link {
    struct Sent {
        unsigned port;
        Peer to;
        std::vector<std::uint8_t> bytes;
    };
    void send(unsigned port, const Peer& to, std::span<const std::uint8_t> b) override {
        sent.push_back({port, to, {b.begin(), b.end()}});
    }
    std::vector<Sent> on(unsigned port) const {
        std::vector<Sent> out;
        for (const Sent& s : sent) {
            if (s.port == port) {
                out.push_back(s);
            }
        }
        return out;
    }
    std::vector<Sent> sent;
};

const Peer kA{0x0a000001, 50000, false};
const Peer kB{0x0a000002, 50001, false};

TxPolicyOptions transmit() {
    TxPolicyOptions o;
    o.allow_tx = true;
    o.allow_pa = true;
    o.drive_ceiling = 255;
    return o;
}

std::int32_t sample24(const std::uint8_t* p) {
    const std::uint32_t u =
        static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
    return static_cast<std::int32_t>(u << 8) >> 8;
}

struct Rig {
    explicit Rig(TxPolicyOptions o = {}) : policy(control, session, o) {}
    Control control;
    SpscRing<TxBlock> tx{4};
    FakeLink link;
    p1::GatewareStatus status;
    Session session{[this] { control.safe(); }, 3s};
    TxPolicy policy;
    Server server{Identity{{2, 0, 0, 0, 0, 1}, 75, 6}, control, tx, session, policy,
                  [this] { return status; },           link};
    std::uint32_t hp_sequence = 0;

    void packet(unsigned port, const std::vector<std::uint8_t>& p, const Peer& from = kA,
                std::chrono::nanoseconds now = 0s) {
        server.on_packet(port, p, from, now);
    }
    void high_priority(const HighPriorityBytes& h, const Peer& from = kA,
                       std::chrono::nanoseconds now = 0s) {
        packet(kHighPriorityPort, high_priority_packet(hp_sequence++, h), from, now);
    }
    // General, DDCs and run, as piHPSDR starts.
    void start(std::initializer_list<DdcRequest> ddcs, HighPriorityBytes h = {}) {
        packet(kGeneralPort, general_packet(0));
        packet(kRxSpecificPort, rx_specific_packet(0, ddcs));
        packet(kTxSpecificPort, tx_specific_packet(0));
        high_priority(h);
    }
    // `frames` frames at the device's layout, in blocks as large as fit;
    // receiver r's I is r * 0x10000 + `value`, its Q the negative.
    void rx(std::size_t frames, std::int32_t value = 0) {
        const std::size_t most = RxBlock::kCapacity / rx_frame_bytes(server.device_receivers());
        for (std::size_t done = 0; done < frames; done += most) {
            rx_block(std::min(most, frames - done), value);
        }
    }
    void rx_block(std::size_t frames, std::int32_t value) {
        RxBlock b;
        b.receivers = server.device_receivers();
        b.rate_code = server.device_rate_code();
        b.size = frames * rx_frame_bytes(b.receivers);
        for (std::size_t f = 0; f < frames; ++f) {
            for (unsigned r = 0; r < b.receivers; ++r) {
                std::uint8_t* p = b.bytes.data() + f * rx_frame_bytes(b.receivers) + r * 6;
                const std::int32_t i = static_cast<std::int32_t>(r) * 0x10000 + value;
                const std::int32_t q = -i;
                p[0] = static_cast<std::uint8_t>(i >> 16);
                p[1] = static_cast<std::uint8_t>(i >> 8);
                p[2] = static_cast<std::uint8_t>(i);
                p[3] = static_cast<std::uint8_t>(q >> 16);
                p[4] = static_cast<std::uint8_t>(q >> 8);
                p[5] = static_cast<std::uint8_t>(q);
            }
        }
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

TEST_CASE("Protocol 2 discovery is answered from the base port", "[p2server]") {
    Rig rig;
    rig.packet(kGeneralPort, p2_discovery_packet(), kB);
    REQUIRE(rig.link.sent.size() == 1);
    CHECK(rig.link.sent[0].port == kGeneralPort);
    CHECK(rig.link.sent[0].to == kB);
    CHECK(rig.link.sent[0].bytes[4] == 0x02);
    CHECK(rig.link.sent[0].bytes[20] == 6);
    rig.packet(kGeneralPort, general_packet(0), kA);  // kA owns the radio now
    rig.packet(kGeneralPort, p2_discovery_packet(), kB);
    CHECK(rig.link.sent.back().bytes[4] == 0x03);
}

TEST_CASE("one Protocol 2 client owns the radio until it stops", "[p2server]") {
    Rig rig;
    rig.start({{0, 48}});
    CHECK(rig.server.running());
    REQUIRE(rig.server.client());
    CHECK(*rig.server.client() == kA);
    CHECK(rig.session.owns({Protocol::P2, kA}));

    // Another client is ignored, on every port.
    const auto ignored = rig.server.counters().ignored;
    rig.packet(kGeneralPort, general_packet(0), kB);
    rig.packet(kRxSpecificPort, rx_specific_packet(0, {{0, 384}}), kB);
    rig.packet(kTxSpecificPort, tx_specific_packet(0), kB);
    rig.high_priority({}, kB);
    rig.packet(kAudioPort, std::vector<std::uint8_t>(260, 0), kB);
    rig.packet(kTxIqPort, tx_iq_packet(0), kB);
    rig.packet(9, std::vector<std::uint8_t>(60, 0), kA);  // not a port packets arrive on
    rig.packet(kHighPriorityPort, std::vector<std::uint8_t>(60, 0), kA);  // malformed
    CHECK(rig.server.counters().ignored == ignored + 8);
    CHECK(rig.server.device_rate_code() == 0);

    // Its audio keeps it heard.
    rig.packet(kAudioPort, std::vector<std::uint8_t>(260, 0), kA, 2s);
    rig.session.tick(4s);
    rig.server.tick();
    CHECK(rig.server.running());

    // Run clear releases the radio and makes the gateware safe.
    HighPriorityBytes stop;
    stop.run = false;
    rig.high_priority(stop, kA, 4s);
    CHECK_FALSE(rig.server.running());
    CHECK_FALSE(rig.server.client());
    CHECK_FALSE(rig.session.owner());
    CHECK(rig.control.value(0x09) == 0u);

    // A run clear from someone not owning it changes nothing.
    rig.high_priority(stop, kB, 5s);
    CHECK_FALSE(rig.session.owner());

    // Now kB may take it; a Protocol 1 client may not meanwhile.
    rig.start({{0, 48}});
    rig.session.release({Protocol::P2, kA}, "test");
    rig.packet(kGeneralPort, general_packet(0), kB, 6s);
    CHECK(rig.session.owns({Protocol::P2, kB}));
    CHECK_FALSE(rig.session.claim({Protocol::P1, kA}, 6s));
}

TEST_CASE("a silent Protocol 2 client loses the radio", "[p2server]") {
    Rig rig;
    rig.start({{0, 48}});
    rig.session.tick(4s);
    rig.server.tick();
    CHECK_FALSE(rig.server.running());
    rig.rx(238);
    CHECK(rig.link.sent.empty());
}

TEST_CASE("a Protocol 1 owner keeps Protocol 2 out", "[p2server]") {
    Rig rig;
    REQUIRE(rig.session.claim({Protocol::P1, kA}, 0s));
    rig.start({{0, 48}});
    CHECK_FALSE(rig.server.running());
    CHECK(rig.server.counters().ignored == 4);
}

TEST_CASE("DDC requests set the gateware's receivers and rate", "[p2server]") {
    Rig rig;
    rig.start({{0, 192}, {1, 48}});
    CHECK(rig.server.device_rate_code() == 2);
    CHECK(rig.server.device_receivers() == 2);
    // Rate 192 ksps, two receivers, duplex.
    CHECK(rig.control.value(Control::kGeneral) == (2u << 24 | 1u << 3 | 1u << 2));

    // Nothing enabled, as while transmitting without duplex: kept.
    rig.packet(kRxSpecificPort, rx_specific_packet(1, {}));
    CHECK(rig.server.device_receivers() == 2);
    CHECK(rig.server.device_rate_code() == 2);

    // Rates the gateware lacks and DDCs beyond its receivers are not
    // served; synchronised DDCs and other ADCs are reported, and served.
    rig.packet(kRxSpecificPort, rx_specific_packet(2, {{0, 1536}, {1, 96, 1, 0x01}, {6, 48}}));
    CHECK(rig.server.device_rate_code() == 1);
    CHECK(rig.server.device_receivers() == 2);

    // Six DDCs at 384 ksps: the bus carries three.
    rig.packet(kRxSpecificPort,
               rx_specific_packet(3, {{0, 384}, {1, 384}, {2, 384}, {3, 384}, {4, 384}, {5, 384}}));
    CHECK(rig.server.device_rate_code() == 3);
    CHECK(rig.server.device_receivers() == 3);
    rig.rx(238);
    // DDC3 to DDC5 are sent, as zero.
    const auto ddc5 = rig.link.on(kDdcPort0 + 5);
    REQUIRE(ddc5.size() == 1);
    CHECK(sample24(ddc5[0].bytes.data() + kDdcHeaderBytes) == 0);
    CHECK(rig.link.on(kDdcPort0 + 2).size() == 1);
    CHECK(rig.server.counters().ddc_packets_out == 6);
}

TEST_CASE("High Priority sets frequencies, gain, drive and MOX", "[p2server]") {
    Rig rig(transmit());
    rig.server.set_frequency_ppm(22.9);
    HighPriorityBytes h;
    h.ddc = {phase_word(7074000), phase_word(14074000)};
    h.duc = phase_word(7074000);
    h.drive = 0x90;
    rig.packet(kGeneralPort, general_packet(0, true));
    rig.packet(kRxSpecificPort, rx_specific_packet(0, {{0, 48}, {1, 48}}));
    rig.high_priority(h);
    CHECK(rig.control.value(0x02) == corrected_frequency(7074000, 22.9));
    CHECK(rig.control.value(0x03) == corrected_frequency(14074000, 22.9));
    CHECK_FALSE(rig.control.value(0x04));  // DDC2 unset: not sent
    CHECK(rig.control.value(0x01) == corrected_frequency(7074000, 22.9));
    CHECK(rig.control.value(0x0a) == (0x40u | 26u));  // 14 dB
    CHECK(rig.policy.effective_tx_drive(false) == 0x90u << 24);
    CHECK(rig.policy.effective_tx_drive(true) == (0x90u << 24 | 1u << 19));
    CHECK_FALSE(rig.policy.transmitting());

    // Frequencies in Hz when the General packet says so.
    rig.packet(kGeneralPort, general_packet(1, false, false));
    h.ddc = {3573000};
    h.duc = 3573000;
    h.attenuation = 10;
    h.mox = true;
    h.cwx = true;
    rig.server.set_frequency_ppm(0);
    rig.high_priority(h);
    CHECK(rig.control.value(0x02) == 3573000u);
    CHECK(rig.control.value(0x01) == 3573000u);
    CHECK(rig.control.value(0x0a) == (0x40u | 16u));  // 14 - 10 dB
    CHECK(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{true, true, true});
    CHECK(rig.control.value(0x09) == 0x90u << 24);  // PA now off

    // Gain floors at -12 dB; the base gain is set by the daemon.
    h.attenuation = 31;
    rig.high_priority(h);
    CHECK(rig.control.value(0x0a) == 0x40u);
    rig.server.set_rx_gain_db(60);
    h.attenuation = 0;
    rig.high_priority(h);
    CHECK(rig.control.value(0x0a) == (0x40u | 60u));  // 48 dB, the most
    h.mox = false;
    rig.high_priority(h);
    CHECK_FALSE(rig.policy.transmitting());
}

TEST_CASE("DDC I/Q streams at the device rate", "[p2server]") {
    Rig rig;
    rig.start({{0, 48}, {1, 48}});
    rig.drain();
    rig.status = {true, false};
    rig.rx(200, 5);
    CHECK(rig.link.on(kDdcPort0).empty());
    rig.rx(38 + 238, 5);
    const auto ddc0 = rig.link.on(kDdcPort0);
    const auto ddc1 = rig.link.on(kDdcPort0 + 1);
    REQUIRE(ddc0.size() == 2);
    REQUIRE(ddc1.size() == 2);
    CHECK(ddc0[0].to == kA);
    CHECK(ddc0[0].bytes.size() == kLongPacketBytes);
    CHECK(ddc0[0].bytes[3] == 0);
    CHECK(ddc0[1].bytes[3] == 1);
    CHECK(ddc0[0].bytes[15] == 238);
    const std::uint8_t* s = ddc0[0].bytes.data() + kDdcHeaderBytes;
    CHECK(sample24(s) == 5);
    CHECK(sample24(s + 3) == -5);
    CHECK(sample24(s + 237 * 6) == 5);
    CHECK(sample24(ddc1[0].bytes.data() + kDdcHeaderBytes) == 0x10005);

    // The standard convention swaps I and Q.
    rig.server.set_iq_convention(p1::IqConvention::Standard);
    rig.rx(238, 5);
    const auto swapped = rig.link.on(kDdcPort0);
    REQUIRE(swapped.size() == 3);
    CHECK(sample24(swapped[2].bytes.data() + kDdcHeaderBytes) == -5);

    // A block at another rate (in a change) is not streamed.
    RxBlock other;
    other.rate_code = 1;
    other.receivers = 2;
    other.size = 238 * rx_frame_bytes(2);
    rig.server.on_rx(other);
    CHECK(rig.link.on(kDdcPort0).size() == 3);
}

TEST_CASE("a DDC slower than the device is decimated", "[p2server]") {
    Rig rig;
    rig.start({{0, 96}, {1, 48}});
    REQUIRE(rig.server.device_rate_code() == 1);
    // DC through the filter: the level is kept.
    for (int i = 0; i < 8; ++i) {
        rig.rx(476, 0x1000);
    }
    const auto ddc0 = rig.link.on(kDdcPort0);
    const auto ddc1 = rig.link.on(kDdcPort0 + 1);
    REQUIRE(ddc0.size() == 16);
    REQUIRE(ddc1.size() == 8);
    const std::uint8_t* last = ddc1.back().bytes.data() + kDdcHeaderBytes + 100 * 6;
    CHECK(std::abs(sample24(last) - 0x11000) <= 2);
    CHECK(std::abs(sample24(last + 3) + 0x11000) <= 2);
}

TEST_CASE("Mic/Line and status packets follow the radio's clock", "[p2server]") {
    Rig rig;
    rig.start({{0, 192}});
    rig.rx(255);
    CHECK(rig.link.on(kMicLinePort).empty());
    rig.rx(1);  // 256 frames at 192 ksps: 64 samples at 48 kHz
    const auto mic = rig.link.on(kMicLinePort);
    REQUIRE(mic.size() == 1);
    CHECK(mic[0].bytes.size() == kMicPacketBytes);
    CHECK(mic[0].bytes[3] == 0);

    // Status: at once on the first block, then each 50 ms of samples.
    auto status = rig.link.on(kHighPriorityStatusPort);
    REQUIRE(status.size() == 1);
    CHECK(status[0].bytes.size() == kShortPacketBytes);
    CHECK(status[0].bytes[4] == 0);
    rig.rx(9600 - 256);
    CHECK(rig.link.on(kHighPriorityStatusPort).size() == 1);
    rig.rx(256);
    CHECK(rig.link.on(kHighPriorityStatusPort).size() == 2);
    // And at once when the inputs change.
    rig.status = {true, true};
    rig.rx(1);
    status = rig.link.on(kHighPriorityStatusPort);
    REQUIRE(status.size() == 3);
    CHECK(status[2].bytes[3] == 2);
    CHECK(status[2].bytes[4] == 0x03);
}

TEST_CASE("TX I/Q is decimated to 48 ksps for the gateware", "[p2server]") {
    Rig rig(transmit());
    rig.start({{0, 48}});
    for (std::uint32_t seq = 0; seq < 4; ++seq) {
        rig.packet(kTxIqPort, tx_iq_packet(seq, 0x123400, -0x123400));
        TxBlock* b = rig.tx.front();
        REQUIRE(b != nullptr);
        CHECK(b->size == 60 * kTxSampleBytes);
        if (seq == 3) {
            // Settled: 24-bit full scale maps to 16-bit full scale.
            const std::uint8_t* last = b->bytes.data() + 59 * kTxSampleBytes;
            const auto i = static_cast<std::int16_t>(last[0] << 8 | last[1]);
            const auto q = static_cast<std::int16_t>(last[2] << 8 | last[3]);
            CHECK(std::abs(i - 0x1234) <= 1);
            CHECK(std::abs(q + 0x1234) <= 1);
        }
        rig.tx.pop();
    }
    rig.server.set_iq_convention(p1::IqConvention::Standard);
    rig.packet(kTxIqPort, tx_iq_packet(4, 0x123400, -0x123400));
    const std::uint8_t* last = rig.tx.front()->bytes.data() + 59 * kTxSampleBytes;
    CHECK(static_cast<std::int16_t>(last[0] << 8 | last[1]) < 0);  // Q first
    rig.tx.pop();
    CHECK(rig.server.counters().tx_iq_sequence_gaps == 0);
}

TEST_CASE("a TX I/Q gap or a full queue while transmitting drops MOX", "[p2server]") {
    Rig rig(transmit());
    HighPriorityBytes h;
    h.mox = true;
    rig.start({{0, 48}}, h);
    REQUIRE(rig.policy.transmitting());
    rig.packet(kTxIqPort, tx_iq_packet(0));
    rig.packet(kTxIqPort, tx_iq_packet(2));  // 1 lost
    CHECK(rig.server.counters().tx_iq_sequence_gaps == 1);
    CHECK(rig.session.latched());
    CHECK(rig.control.transmit() == TransmitState{});

    // Released and keyed again; then the queue (4 blocks) overflows.
    h.mox = false;
    rig.high_priority(h);
    h.mox = true;
    rig.high_priority(h);
    REQUIRE(rig.policy.transmitting());
    for (std::uint32_t seq = 3; seq < 9; ++seq) {
        rig.packet(kTxIqPort, tx_iq_packet(seq));
    }
    CHECK(rig.server.counters().tx_blocks_dropped > 0);
    CHECK(rig.session.latched());
}

TEST_CASE("unsupported requests are reported and do no harm", "[p2server]") {
    Rig rig;
    auto general = general_packet(0);
    general[6] = 0x05;
    general[23] = 1;
    rig.packet(kGeneralPort, general);
    general[3] = 1;
    rig.packet(kGeneralPort, general);  // reported once
    rig.packet(kTxSpecificPort, tx_specific_packet(0, 48, 16, 0x02));
    rig.packet(kTxSpecificPort, tx_specific_packet(1, 48, 16, 0x02));
    rig.packet(kRxSpecificPort, rx_specific_packet(0, {{0, 48, 1, 0x02}}));
    rig.packet(kRxSpecificPort, rx_specific_packet(5, {{0, 48, 1, 0x02}}));  // a gap
    CHECK(rig.server.counters().command_sequence_gaps == 1);
    CHECK(rig.server.device_receivers() == 1);
    rig.packet(kGeneralPort, std::vector<std::uint8_t>(60, 0xff));  // not General
    CHECK(rig.server.counters().ignored == 1);
}
