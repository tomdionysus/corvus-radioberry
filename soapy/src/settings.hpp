// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "radioberry/device.hpp"
#include "tx_policy.hpp"

namespace radioberry::soapy {

inline constexpr const char* kConfigPath = "/etc/radioberry/config.yaml";

// What the module takes from radioberryd's config file: the same keys, read
// and checked by radioberryd's own parser, so the radio behaves the same
// whichever program drives it.
struct Settings {
    std::string device = kDevicePath;  // `device`
    double frequency_ppm = 0;          // `frequency_ppm`
    int lna_gain_db = 14;              // `p2_rx_gain`, the LNA gain at start
    // `allow_tx`, `allow_pa`, `drive_ceiling`, `tx_watchdog_s`.
    daemon::TxPolicyOptions tx;
    std::string path = kConfigPath;  // the file, for messages
    bool from_file = false;          // the file existed
};

// The settings in the config file at `path`; the defaults (receive only)
// when there is no file. Throws std::invalid_argument, naming the file and
// the problem, for a file radioberryd would refuse.
Settings load_settings(const std::string& path);

// The module's timings; tests shorten them.
struct Timing {
    // After the driver's TX ring has emptied, MOX is held until the
    // gateware's sample counter shows the burst's last sample taken, but
    // not longer than this. Without the counter, MOX is held for tx_tail,
    // the gateware's FIFO (85 ms on the Pi 5) and some.
    std::chrono::milliseconds tx_tail_max{250};
    std::chrono::milliseconds tx_tail{100};
    // How often the transmit watchdog and the burst end are checked while
    // a burst is open.
    std::chrono::milliseconds tx_tick{20};
    // Longest wait for a burst to drain when the TX stream is deactivated.
    std::chrono::milliseconds tx_drain{1000};
    // Samples queued for transmission at most (the module's queue and the
    // driver's ring): 100 ms at 48 ksps, as radioberryd keeps it.
    std::uint32_t tx_queue_samples = 4800;
};

}  // namespace radioberry::soapy
