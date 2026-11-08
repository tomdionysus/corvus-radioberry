// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace radioberry::daemon {

// What the PIO bus carries from the gateware: about 2.37 million 24-bit
// values per second, measured (gateware/README.md section 5). One
// receiver at rate R needs 2R values per second.
inline constexpr std::uint32_t kRxValuesPerSecondMax = 2'370'000;

// Sample rate for a Hermes-Lite 2 rate code (0..3).
constexpr std::uint32_t rx_rate_hz(unsigned rate_code) { return 48000u << (rate_code & 3u); }

// Most receivers the device runs at `rate_code`: the gateware's count,
// limited by the bus (six at 192 ksps, three at 384 ksps).
constexpr unsigned max_receivers(unsigned rate_code, unsigned gateware_receivers) {
    return std::min<unsigned>(gateware_receivers,
                              kRxValuesPerSecondMax / (2 * rx_rate_hz(rate_code)));
}

}  // namespace radioberry::daemon
