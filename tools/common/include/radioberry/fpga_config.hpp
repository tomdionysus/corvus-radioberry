// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace radioberry {

// Raspberry Pi GPIO lines wired to the FPGA's passive serial configuration
// port on the RadioBerry 2.x.
struct ConfigLines {
    static constexpr unsigned kData = 13;
    static constexpr unsigned kDclk = 24;
    static constexpr unsigned kNConfig = 27;
    static constexpr unsigned kConfDone = 22;
    static constexpr unsigned kNStatus = 26;
};

// The five configuration lines, abstracted so the loader can be tested
// against a simulated FPGA.
class ConfigPins {
public:
    virtual ~ConfigPins() = default;

    virtual void set_nconfig(bool high) = 0;
    virtual void set_data(bool high) = 0;
    virtual void set_dclk(bool high) = 0;
    virtual bool nstatus() = 0;
    virtual bool conf_done() = 0;
    virtual void delay(std::chrono::microseconds duration) = 0;
};

enum class ConfigError {
    kNone,
    kNStatusStuckHigh,  // nSTATUS did not go low while nCONFIG was low
    kNStatusStuckLow,   // nSTATUS did not release after nCONFIG went high
    kCrcError,          // nSTATUS went low during or after the bitstream
    kConfDoneLow,       // whole bitstream sent, CONF_DONE still low
};

struct ConfigResult {
    ConfigError error = ConfigError::kNone;
    bool ok() const { return error == ConfigError::kNone; }
    std::string message() const;
};

// Configures the FPGA through passive serial with an .rbf image: pulse
// nCONFIG, wait for nSTATUS, clock the bytes least significant bit first,
// then check nSTATUS and CONF_DONE and clock the initialisation cycles.
ConfigResult configure_passive_serial(ConfigPins& pins, std::span<const std::uint8_t> image);

// The GPIO chip carrying the 40-pin header's GPIOs (RP1 on a Pi 5, BCM2711
// on a Pi 4), or nothing.
std::optional<std::filesystem::path> find_header_gpio_chip();

// Live pins through libgpiod on the header's GPIO chip. Throws
// std::runtime_error when the lines cannot be requested, typically because
// the kernel driver owns them.
std::unique_ptr<ConfigPins> open_gpiod_config_pins();

}  // namespace radioberry
