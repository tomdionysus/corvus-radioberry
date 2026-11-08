// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace radioberry::daemon {

// True if a Hermes-Lite 2 command carries a frequency in Hz: TX (0x01),
// receivers 1 to 7 (0x02 to 0x08) and 8 to 12 (0x12 to 0x16).
constexpr bool is_frequency_address(std::uint8_t address) {
    return address == 0x01 || (address >= 0x02 && address <= 0x08) ||
           (address >= 0x12 && address <= 0x16);
}

// The value to command so that a radio whose frequency error is `ppm`
// (positive: it tunes high) tunes to `hz`.
inline std::uint32_t corrected_frequency(std::uint32_t hz, double ppm) {
    const double value = std::round(static_cast<double>(hz) / (1.0 + ppm * 1e-6));
    if (value <= 0) {
        return 0;
    }
    if (value >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace radioberry::daemon
