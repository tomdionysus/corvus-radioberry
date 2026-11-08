// SPDX-License-Identifier: GPL-2.0-or-later
#include "p1/protocol1.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace radioberry::daemon::p1 {
namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 |
           std::uint32_t{p[3]};
}

void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

bool has_sync(const std::uint8_t* half) {
    return half[0] == 0x7f && half[1] == 0x7f && half[2] == 0x7f;
}

HostCommand command_at(const std::uint8_t* c) {
    HostCommand h;
    h.address = static_cast<std::uint8_t>((c[0] >> 1) & 0x3fu);
    h.mox = (c[0] & 0x01u) != 0;
    h.ack = (c[0] & 0x80u) != 0;
    h.data = be32(c + 1);
    return h;
}

}  // namespace

PacketKind classify(std::span<const std::uint8_t> p) {
    if (p.size() < 4 || p[0] != 0xef || p[1] != 0xfe) {
        return PacketKind::Unknown;
    }
    switch (p[2]) {
        case 0x02:
            return PacketKind::Discovery;
        case 0x04:
            return PacketKind::StartStop;
        case 0x01:
            return p[3] == 0x02 ? PacketKind::Data : PacketKind::Unknown;
        default:
            return PacketKind::Unknown;
    }
}

std::array<std::uint8_t, kDiscoveryReplyBytes> discovery_reply(const Identity& id, bool running) {
    std::array<std::uint8_t, kDiscoveryReplyBytes> r{};
    r[0] = 0xef;
    r[1] = 0xfe;
    r[2] = running ? 0x03 : 0x02;
    std::copy(id.mac.begin(), id.mac.end(), r.begin() + 3);
    r[9] = id.version_major;
    r[10] = kBoardHermesLite;
    r[19] = id.receivers;
    r[21] = id.version_minor;
    return r;
}

std::optional<bool> parse_start(std::span<const std::uint8_t> p) {
    if (classify(p) != PacketKind::StartStop) {
        return std::nullopt;
    }
    return (p[3] & 0x01u) != 0;
}

std::optional<HostPacket> parse_host_packet(std::span<const std::uint8_t> p) {
    if (p.size() != kPacketBytes || classify(p) != PacketKind::Data) {
        return std::nullopt;
    }
    const std::uint8_t* halves[2] = {p.data() + 8, p.data() + 8 + kHalfBytes};
    if (!has_sync(halves[0]) || !has_sync(halves[1])) {
        return std::nullopt;
    }
    HostPacket h;
    h.sequence = be32(p.data() + 4);
    std::size_t out = 0;
    for (std::size_t half = 0; half < 2; ++half) {
        h.commands[half] = command_at(halves[half] + 3);
        for (std::size_t s = 0; s < kTxSamplesPerPacket / 2; ++s) {
            const std::uint8_t* sample = halves[half] + 8 + 8 * s;
            std::copy(sample + 4, sample + 8, h.tx_iq.begin() + static_cast<std::ptrdiff_t>(out));
            out += 4;
        }
    }
    return h;
}

Word status_word(unsigned address, bool ptt, bool cw_key, std::uint8_t version_major) {
    Word w{};
    w[0] = static_cast<std::uint8_t>((address & 0x03u) << 3 | (cw_key ? 0x04u : 0u) |
                                     (ptt ? 0x01u : 0u));
    if ((address & 0x03u) == 0) {
        w[1] = 0x1e;
        w[4] = version_major;
    }
    return w;
}

Word ack_word(std::uint8_t address, bool ptt, std::uint32_t data) {
    Word w{};
    w[0] = static_cast<std::uint8_t>(0x80u | (address & 0x3fu) << 1 | (ptt ? 0x01u : 0u));
    put_be32(w.data() + 1, data);
    return w;
}

void RxPacker::set_receivers(unsigned receivers) {
    if (receivers < 1 || receivers > kMaxClientReceivers) {
        throw std::out_of_range("receiver count " + std::to_string(receivers));
    }
    if (receivers != receivers_) {
        receivers_ = receivers;
        begin_packet();
    }
}

void RxPacker::reset() {
    sequence_ = 0;
    begin_packet();
}

void RxPacker::begin_packet() {
    packet_.fill(0);
    packet_[0] = 0xef;
    packet_[1] = 0xfe;
    packet_[2] = 0x01;
    packet_[3] = 0x06;
    for (std::size_t half = 0; half < 2; ++half) {
        std::fill_n(packet_.begin() + static_cast<std::ptrdiff_t>(8 + half * kHalfBytes), 3, 0x7f);
    }
    half_ = 0;
    sample_ = 0;
}

void RxPacker::add(const RxBlock& block, const std::function<Word()>& word,
                   const std::function<void(std::span<const std::uint8_t>)>& emit) {
    const std::size_t frame = rx_frame_bytes(block.receivers);
    const std::size_t sample_bytes = 6u * receivers_ + 2u;
    const std::size_t per_half = samples_per_half(receivers_);
    const std::size_t copy = 6u * std::min(receivers_, block.receivers);
    for (std::size_t f = 0; f + frame <= block.size; f += frame) {
        std::uint8_t* out = packet_.data() + 8 + half_ * kHalfBytes + 8 + sample_ * sample_bytes;
        std::copy_n(block.bytes.begin() + static_cast<std::ptrdiff_t>(f), copy, out);
        if (++sample_ < per_half) {
            continue;
        }
        sample_ = 0;
        if (++half_ < 2) {
            continue;
        }
        put_be32(packet_.data() + 4, sequence_++);
        for (std::size_t half = 0; half < 2; ++half) {
            const Word w = word();
            std::copy(w.begin(), w.end(),
                      packet_.begin() + static_cast<std::ptrdiff_t>(8 + half * kHalfBytes + 3));
        }
        emit(packet_);
        begin_packet();
    }
}

}  // namespace radioberry::daemon::p1
