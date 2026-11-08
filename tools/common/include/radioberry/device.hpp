// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <radioberry_ioctl.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "radioberry/command.hpp"

namespace radioberry {

// /dev/radioberry, behind an interface so tools and the daemon are tested
// against fakes. Failures throw std::system_error.
class Device {
public:
    virtual ~Device() = default;

    virtual radioberry_info info() = 0;
    virtual radioberry_stats stats() = 0;
    // Sends one command and returns the gateware's reply.
    virtual Reply command(const Command& command) = 0;
    // Reads whole RX frames into `buffer`, at least one frame long. Returns
    // the bytes read; 0 when interrupted by a signal.
    virtual std::size_t read(std::span<std::uint8_t> buffer) = 0;
    // Writes whole TX samples. Returns the bytes written; 0 when
    // interrupted by a signal.
    virtual std::size_t write(std::span<const std::uint8_t> buffer) = 0;
};

inline constexpr const char* kDevicePath = "/dev/radioberry";

// Opens the character device read-write, blocking.
std::unique_ptr<Device> open_device(const std::string& path);

// Bytes per RX frame for `receivers` receivers: 24-bit I and Q each.
constexpr std::size_t rx_frame_bytes(unsigned receivers) { return 6u * receivers; }
// Bytes per TX sample: 16-bit I and Q.
inline constexpr std::size_t kTxSampleBytes = 4;

}  // namespace radioberry
