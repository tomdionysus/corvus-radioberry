// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace radioberry::daemon {

// What the Pi 5's PIO bus carries from the gateware: about 2.37 million
// 24-bit values per second, measured (gateware/README.md section 5). The
// driver reports its own figure (radioberry_info.rx_values_max), lower on a
// Pi 4; drivers up to release 0.3.0 report 0, meaning this. One receiver at rate R
// needs 2R values per second.
inline constexpr std::uint32_t kRxValuesPerSecondMax = 2'370'000;

// Sample rate for a Hermes-Lite 2 rate code (0..3).
constexpr std::uint32_t rx_rate_hz(unsigned rate_code) { return 48000u << (rate_code & 3u); }

// Most receivers the device runs at `rate_code`: the gateware's count,
// limited by what the bus carries, `values_max` per second (0: the Pi 5's
// kRxValuesPerSecondMax). At least one, even where one is more than the bus
// carries: a client's rate cannot be refused, only its extra receivers.
constexpr unsigned max_receivers(unsigned rate_code, unsigned gateware_receivers,
                                 std::uint32_t values_max = 0) {
    const std::uint32_t carried = values_max ? values_max : kRxValuesPerSecondMax;
    return std::max(1u,
                    std::min<unsigned>(gateware_receivers, carried / (2 * rx_rate_hz(rate_code))));
}

}  // namespace radioberry::daemon
