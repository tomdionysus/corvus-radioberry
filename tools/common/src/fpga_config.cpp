// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/fpga_config.hpp"

namespace radioberry {
namespace {

using std::chrono::microseconds;

// Cyclone 10 LP: nSTATUS follows nCONFIG low within tSTATUS and is released
// within 1506 us of nCONFIG going high. Poll in steps well inside that.
constexpr microseconds kPollStep{5};
constexpr int kPollLimit = 1000;
// Clock cycles after CONF_DONE for initialisation. The device needs only a
// few with its internal oscillator; extra cycles are ignored.
constexpr int kInitClocks = 64;

bool wait_for_nstatus(ConfigPins& pins, bool level) {
    for (int i = 0; i < kPollLimit; ++i) {
        if (pins.nstatus() == level) {
            return true;
        }
        pins.delay(kPollStep);
    }
    return false;
}

}  // namespace

std::string ConfigResult::message() const {
    switch (error) {
        case ConfigError::kNone:
            return "configured";
        case ConfigError::kNStatusStuckHigh:
            return "nSTATUS stayed high while nCONFIG was low";
        case ConfigError::kNStatusStuckLow:
            return "nSTATUS stayed low after nCONFIG was released";
        case ConfigError::kCrcError:
            return "nSTATUS went low: the FPGA rejected the bitstream";
        case ConfigError::kConfDoneLow:
            break;
    }
    return "CONF_DONE stayed low: image does not match this FPGA";
}

ConfigResult configure_passive_serial(ConfigPins& pins, std::span<const std::uint8_t> image) {
    pins.set_dclk(false);
    pins.set_data(false);
    pins.set_nconfig(false);
    pins.delay(microseconds{2});
    if (!wait_for_nstatus(pins, false)) {
        pins.set_nconfig(true);
        return {ConfigError::kNStatusStuckHigh};
    }
    pins.set_nconfig(true);
    if (!wait_for_nstatus(pins, true)) {
        return {ConfigError::kNStatusStuckLow};
    }
    pins.delay(microseconds{2});

    bool data = false;
    for (const std::uint8_t byte : image) {
        for (int bit = 0; bit < 8; ++bit) {
            const bool level = ((byte >> bit) & 1U) != 0;
            if (level != data) {
                pins.set_data(level);
                data = level;
            }
            pins.set_dclk(true);
            pins.set_dclk(false);
        }
    }
    pins.set_data(false);

    if (!pins.nstatus()) {
        return {ConfigError::kCrcError};
    }
    if (!pins.conf_done()) {
        return {ConfigError::kConfDoneLow};
    }
    for (int i = 0; i < kInitClocks; ++i) {
        pins.set_dclk(true);
        pins.set_dclk(false);
    }
    return {};
}

}  // namespace radioberry
