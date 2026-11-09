// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2/protocol2.hpp"

#include <cmath>

namespace radioberry::daemon::p2 {
namespace {

std::uint16_t be16(std::span<const std::uint8_t> p, std::size_t at) {
    return static_cast<std::uint16_t>(p[at] << 8 | p[at + 1]);
}

std::uint32_t be32(std::span<const std::uint8_t> p, std::size_t at) {
    return static_cast<std::uint32_t>(p[at]) << 24 | static_cast<std::uint32_t>(p[at + 1]) << 16 |
           static_cast<std::uint32_t>(p[at + 2]) << 8 | p[at + 3];
}

void put_be16(std::span<std::uint8_t> p, std::size_t at, std::uint16_t v) {
    p[at] = static_cast<std::uint8_t>(v >> 8);
    p[at + 1] = static_cast<std::uint8_t>(v);
}

void put_be32(std::span<std::uint8_t> p, std::size_t at, std::uint32_t v) {
    p[at] = static_cast<std::uint8_t>(v >> 24);
    p[at + 1] = static_cast<std::uint8_t>(v >> 16);
    p[at + 2] = static_cast<std::uint8_t>(v >> 8);
    p[at + 3] = static_cast<std::uint8_t>(v);
}

}  // namespace

std::uint32_t sequence_of(std::span<const std::uint8_t> packet) {
    return packet.size() >= 4 ? be32(packet, 0) : 0;
}

ShortPacket discovery_reply(const Identity& id, bool running) {
    ShortPacket p{};
    p[4] = running ? 0x03 : 0x02;
    for (std::size_t i = 0; i < id.mac.size(); ++i) {
        p[5 + i] = id.mac[i];
    }
    p[11] = kBoardHermesLite;
    p[12] = kProtocolVersion;
    p[13] = id.version_major;
    p[20] = id.receivers;
    return p;
}

bool is_general(std::span<const std::uint8_t> packet) {
    return packet.size() == kShortPacketBytes && packet[4] == 0x00;
}

std::optional<General> parse_general(std::span<const std::uint8_t> packet) {
    if (!is_general(packet)) {
        return std::nullopt;
    }
    General g;
    g.sequence = be32(packet, 0);
    for (std::size_t i = 5; i <= 22; ++i) {
        g.default_ports = g.default_ports && packet[i] == 0;
    }
    g.wideband = packet[23] != 0;
    g.phase_words = (packet[37] & 0x08) != 0;
    g.pa_enable = (packet[58] & 0x01) != 0;
    return g;
}

std::optional<RxSpecific> parse_rx_specific(std::span<const std::uint8_t> packet) {
    if (packet.size() != kLongPacketBytes) {
        return std::nullopt;
    }
    RxSpecific r;
    r.sequence = be32(packet, 0);
    r.adcs = packet[4];
    for (unsigned n = 0; n < kMaxDdcs; ++n) {
        Ddc& d = r.ddc[n];
        d.enabled = ((packet[7 + n / 8] >> (n % 8)) & 1u) != 0;
        d.adc = packet[17 + 6 * n];
        d.rate_ksps = be16(packet, 18 + 6 * n);
        d.bits = packet[22 + 6 * n];
        d.sync = packet[1363 + n];
    }
    return r;
}

std::optional<TxSpecific> parse_tx_specific(std::span<const std::uint8_t> packet) {
    if (packet.size() != kShortPacketBytes) {
        return std::nullopt;
    }
    TxSpecific t;
    t.sequence = be32(packet, 0);
    t.dacs = packet[4];
    t.cw_mode = packet[5];
    t.rate_ksps = be16(packet, 14);
    t.bits = packet[16];
    return t;
}

std::optional<HighPriority> parse_high_priority(std::span<const std::uint8_t> packet) {
    if (packet.size() != kLongPacketBytes) {
        return std::nullopt;
    }
    HighPriority h;
    h.sequence = be32(packet, 0);
    h.run = (packet[4] & 0x01) != 0;
    h.mox = (packet[4] & 0x02) != 0;
    h.cwx = (packet[5] & 0x01) != 0;
    for (unsigned n = 0; n < kHighPriorityDdcs; ++n) {
        h.ddc_frequency[n] = be32(packet, 9 + 4 * n);
    }
    h.duc_frequency = be32(packet, 329);
    h.drive = packet[345];
    h.open_collector = packet[1401];
    h.attenuation_adc1 = packet[1442];
    h.attenuation_adc0 = packet[1443];
    return h;
}

std::uint32_t frequency_from_phase_word(std::uint32_t phase) {
    const double hz = std::round(static_cast<double>(phase) * 122880000.0 / 4294967296.0);
    return static_cast<std::uint32_t>(hz);
}

std::optional<TxIq> parse_tx_iq(std::span<const std::uint8_t> packet) {
    if (packet.size() != kLongPacketBytes) {
        return std::nullopt;
    }
    return TxIq{be32(packet, 0), packet.subspan(4, kTxSamplesPerPacket * kSampleBytes)};
}

void begin_ddc_packet(LongPacket& packet, std::uint32_t sequence) {
    put_be32(packet, 0, sequence);
    for (std::size_t i = 4; i < 12; ++i) {
        packet[i] = 0;
    }
    put_be16(packet, 12, 24);
    put_be16(packet, 14, static_cast<std::uint16_t>(kDdcSamplesPerPacket));
}

ShortPacket high_priority_status(std::uint32_t sequence, bool ptt, bool cw_key) {
    ShortPacket p{};
    put_be32(p, 0, sequence);
    p[4] = static_cast<std::uint8_t>((ptt ? 0x01 : 0) | (cw_key ? 0x02 : 0));
    return p;
}

MicPacket mic_packet(std::uint32_t sequence) {
    MicPacket p{};
    put_be32(p, 0, sequence);
    return p;
}

}  // namespace radioberry::daemon::p2
