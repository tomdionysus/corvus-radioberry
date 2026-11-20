// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A free range of kPortStride ports on loopback for one daemon under test:
// Protocol 1 takes the base for UDP and TCP, Protocol 2 the base and its
// offsets. Taken below the kernel's ephemeral range (32768 up by default),
// where other programs' client sockets live, starting from a block chosen
// by pid so test processes run in parallel start apart, and skipping any
// range with a port another socket holds, UDP or TCP. A port taken with
// port 0 comes from the ephemeral range, where a TCP bind to the number a
// UDP bind got can find it held.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <stdexcept>

namespace radioberry::test {

inline constexpr int kPortStride = 20;

inline bool port_free(std::uint16_t port, int type) {
    const int fd = ::socket(AF_INET, type, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    const bool ok = ::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    ::close(fd);
    return ok;
}

inline std::uint16_t free_port_block() {
    constexpr int kFirst = 10000;
    constexpr int kBlocks = (32700 - kFirst) / kPortStride;
    static int next = static_cast<int>(::getpid() % kBlocks);
    for (int tries = 0; tries < kBlocks; ++tries) {
        const int block = next;
        next = (next + 1) % kBlocks;
        const auto base = static_cast<std::uint16_t>(kFirst + block * kPortStride);
        bool all = true;
        for (int k = 0; k < kPortStride && all; ++k) {
            const auto p = static_cast<std::uint16_t>(base + k);
            all = port_free(p, SOCK_DGRAM) && port_free(p, SOCK_STREAM);
        }
        if (all) {
            return base;
        }
    }
    throw std::runtime_error("no free port range below 32700");
}

}  // namespace radioberry::test
