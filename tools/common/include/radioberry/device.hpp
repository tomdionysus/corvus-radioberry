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

// What Device::wait found.
struct Readiness {
    bool readable = false;  // at least one RX frame queued
    bool writable = false;  // room for at least one TX sample
    bool error = false;     // an RX overrun not yet reported by stats()
};

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
    // the bytes read; 0 when interrupted by a signal or, opened
    // non-blocking, when no frame is queued.
    virtual std::size_t read(std::span<std::uint8_t> buffer) = 0;
    // Writes whole TX samples. Returns the bytes written; 0 when
    // interrupted by a signal or, opened non-blocking, when the TX ring is
    // full.
    virtual std::size_t write(std::span<const std::uint8_t> buffer) = 0;
    // Waits up to `timeout_ms` (-1: without limit) until a frame can be
    // read (if `want_read`), a sample can be written (if `want_write`) or
    // an RX overrun is pending. Returns all false on timeout or a signal.
    virtual Readiness wait(bool want_read, bool want_write, int timeout_ms) = 0;
};

inline constexpr const char* kDevicePath = "/dev/radioberry";

// Opens the character device read-write, blocking.
std::unique_ptr<Device> open_device(const std::string& path);
// The same, non-blocking: read and write return 0 instead of waiting.
std::unique_ptr<Device> open_device_nonblocking(const std::string& path);

// Bytes per RX frame for `receivers` receivers: 24-bit I and Q each.
constexpr std::size_t rx_frame_bytes(unsigned receivers) { return std::size_t{6} * receivers; }
// Bytes per TX sample: 16-bit I and Q.
inline constexpr std::size_t kTxSampleBytes = 4;

}  // namespace radioberry
