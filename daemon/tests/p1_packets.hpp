// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host-side Protocol 1 packets, built as a client would.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "p1/protocol1.hpp"

namespace radioberry::daemon::test {

inline std::vector<std::uint8_t> discovery_packet(std::size_t size = 63) {
    std::vector<std::uint8_t> p(size, 0);
    p[0] = 0xef;
    p[1] = 0xfe;
    p[2] = 0x02;
    return p;
}

inline std::vector<std::uint8_t> start_packet(std::uint8_t command, std::size_t size = 64) {
    std::vector<std::uint8_t> p(size, 0);
    p[0] = 0xef;
    p[1] = 0xfe;
    p[2] = 0x04;
    p[3] = command;
    return p;
}

// C0 is (address << 1 | mox), plus 0x80 to request an ACK. Every TX
// sample is I = `i`, Q = `q`, with L/R audio 0x5555.
struct HostCommandBytes {
    std::uint8_t c0 = 0;
    std::uint32_t data = 0;
};
inline std::vector<std::uint8_t> host_packet(std::uint32_t sequence, HostCommandBytes first,
                                             HostCommandBytes second, std::uint16_t i = 0,
                                             std::uint16_t q = 0) {
    std::vector<std::uint8_t> p(p1::kPacketBytes, 0);
    p[0] = 0xef;
    p[1] = 0xfe;
    p[2] = 0x01;
    p[3] = 0x02;
    for (int b = 0; b < 4; ++b) {
        p[4 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(sequence >> (24 - 8 * b));
    }
    const HostCommandBytes commands[2] = {first, second};
    for (std::size_t half = 0; half < 2; ++half) {
        std::uint8_t* h = p.data() + 8 + half * p1::kHalfBytes;
        h[0] = h[1] = h[2] = 0x7f;
        h[3] = commands[half].c0;
        for (int b = 0; b < 4; ++b) {
            h[4 + b] = static_cast<std::uint8_t>(commands[half].data >> (24 - 8 * b));
        }
        for (std::size_t s = 0; s < 63; ++s) {
            std::uint8_t* x = h + 8 + 8 * s;
            x[0] = x[1] = x[2] = x[3] = 0x55;
            x[4] = static_cast<std::uint8_t>(i >> 8);
            x[5] = static_cast<std::uint8_t>(i);
            x[6] = static_cast<std::uint8_t>(q >> 8);
            x[7] = static_cast<std::uint8_t>(q);
        }
    }
    return p;
}

}  // namespace radioberry::daemon::test
