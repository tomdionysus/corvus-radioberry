// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host-side Protocol 2 packets, built as piHPSDR's new_protocol.c builds
// them.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "p2/protocol2.hpp"

namespace radioberry::daemon::test {

inline void put_be32(std::vector<std::uint8_t>& p, std::size_t at, std::uint32_t v) {
    for (std::size_t b = 0; b < 4; ++b) {
        p[at + b] = static_cast<std::uint8_t>(v >> (24 - 8 * b));
    }
}

inline std::vector<std::uint8_t> p2_discovery_packet() {
    std::vector<std::uint8_t> p(60, 0);
    p[4] = 0x02;
    return p;
}

inline std::vector<std::uint8_t> general_packet(std::uint32_t sequence, bool pa = false,
                                                bool phase_words = true) {
    std::vector<std::uint8_t> p(p2::kShortPacketBytes, 0);
    put_be32(p, 0, sequence);
    p[37] = phase_words ? 0x08 : 0x00;
    p[38] = 0x01;
    p[58] = pa ? 0x01 : 0x00;
    return p;
}

struct DdcRequest {
    unsigned ddc = 0;
    std::uint16_t rate_ksps = 48;
    std::uint8_t adc = 0;
    std::uint8_t sync = 0;
};

inline std::vector<std::uint8_t> rx_specific_packet(std::uint32_t sequence,
                                                    std::initializer_list<DdcRequest> ddcs) {
    std::vector<std::uint8_t> p(p2::kLongPacketBytes, 0);
    put_be32(p, 0, sequence);
    p[4] = 1;  // ADCs
    for (const DdcRequest& d : ddcs) {
        p[7 + d.ddc / 8] = static_cast<std::uint8_t>(p[7 + d.ddc / 8] | 1u << (d.ddc % 8));
        p[17 + 6 * d.ddc] = d.adc;
        p[18 + 6 * d.ddc] = static_cast<std::uint8_t>(d.rate_ksps >> 8);
        p[19 + 6 * d.ddc] = static_cast<std::uint8_t>(d.rate_ksps);
        p[22 + 6 * d.ddc] = 24;
        p[1363 + d.ddc] = d.sync;
    }
    return p;
}

inline std::vector<std::uint8_t> tx_specific_packet(std::uint32_t sequence,
                                                    std::uint16_t rate_ksps = 0,
                                                    std::uint8_t bits = 0,
                                                    std::uint8_t cw_mode = 0) {
    std::vector<std::uint8_t> p(p2::kShortPacketBytes, 0);
    put_be32(p, 0, sequence);
    p[4] = 1;
    p[5] = cw_mode;
    p[14] = static_cast<std::uint8_t>(rate_ksps >> 8);
    p[15] = static_cast<std::uint8_t>(rate_ksps);
    p[16] = bits;
    return p;
}

// The phase word piHPSDR sends for `hz`.
inline std::uint32_t phase_word(std::uint32_t hz) {
    return static_cast<std::uint32_t>(static_cast<double>(hz) * 34.952533333333333333);
}

struct HighPriorityBytes {
    bool run = true;
    bool mox = false;
    bool cwx = false;
    std::vector<std::uint32_t> ddc;  // as sent: phase words or Hz
    std::uint32_t duc = 0;
    std::uint8_t drive = 0;
    std::uint8_t attenuation = 0;
};

inline std::vector<std::uint8_t> high_priority_packet(std::uint32_t sequence,
                                                      const HighPriorityBytes& h) {
    std::vector<std::uint8_t> p(p2::kLongPacketBytes, 0);
    put_be32(p, 0, sequence);
    p[4] = static_cast<std::uint8_t>((h.run ? 0x01 : 0) | (h.mox ? 0x02 : 0));
    p[5] = h.cwx ? 0x01 : 0x00;
    for (std::size_t n = 0; n < h.ddc.size(); ++n) {
        put_be32(p, 9 + 4 * n, h.ddc[n]);
    }
    put_be32(p, 329, h.duc);
    p[345] = h.drive;
    p[1443] = h.attenuation;
    return p;
}

// 240 samples, every one I = `i`, Q = `q` (24-bit).
inline std::vector<std::uint8_t> tx_iq_packet(std::uint32_t sequence, std::int32_t i = 0,
                                              std::int32_t q = 0) {
    std::vector<std::uint8_t> p(p2::kLongPacketBytes, 0);
    put_be32(p, 0, sequence);
    for (std::size_t s = 0; s < p2::kTxSamplesPerPacket; ++s) {
        std::uint8_t* x = p.data() + 4 + 6 * s;
        x[0] = static_cast<std::uint8_t>(i >> 16);
        x[1] = static_cast<std::uint8_t>(i >> 8);
        x[2] = static_cast<std::uint8_t>(i);
        x[3] = static_cast<std::uint8_t>(q >> 16);
        x[4] = static_cast<std::uint8_t>(q >> 8);
        x[5] = static_cast<std::uint8_t>(q);
    }
    return p;
}

}  // namespace radioberry::daemon::test
