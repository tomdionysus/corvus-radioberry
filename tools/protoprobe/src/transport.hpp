// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace radioberry::proto {

// A datagram from the radio, with the port it came from as an offset from
// the radio's base port (always 0 for Protocol 1).
struct Datagram {
    unsigned offset = 0;
    std::vector<std::uint8_t> bytes;
};

// The client's socket, abstracted so the protocol logic is tested against
// a scripted radio.
class Transport {
public:
    virtual ~Transport() = default;
    // Sends to the radio's base port plus `offset`.
    virtual void send(unsigned offset, std::span<const std::uint8_t> bytes) = 0;
    // The next datagram from the radio, or nothing after `timeout`.
    virtual std::optional<Datagram> receive(std::chrono::milliseconds timeout) = 0;
};

// One UDP socket talking to `host` (IPv4) at `base_port`. Throws
// std::runtime_error or std::system_error.
std::unique_ptr<Transport> open_udp(const std::string& host, std::uint16_t base_port);

}  // namespace radioberry::proto
