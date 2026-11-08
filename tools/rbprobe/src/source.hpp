// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "radioberry/fpga_config.hpp"

namespace radioberry::probe {

struct ConfigStatusLines {
    // False when another consumer (normally the kernel driver) holds the
    // lines; the levels are then not read.
    bool available = false;
    std::string consumer;
    bool conf_done = false;
    bool nstatus = false;
};

struct CommandResult {
    int exit_status = 0;
    std::string output;  // standard output
};

struct I2cDevice {
    std::uint8_t address = 0;
    bool claimed_by_driver = false;
};

// Everything rbprobe reads from the machine. The live implementation reads
// the real system; tests supply fixtures.
class Source {
public:
    virtual ~Source() = default;

    // Whole file contents, or nothing if it cannot be read.
    virtual std::optional<std::string> read_file(const std::string& path) = 0;
    virtual bool exists(const std::string& path) = 0;
    // Entry names, without "." and "..". Empty if not a readable directory.
    virtual std::vector<std::string> list_dir(const std::string& path) = 0;
    virtual std::optional<std::string> read_link(const std::string& path) = 0;
    // Runs a command without a shell. Nothing if it could not be started or
    // did not finish in time.
    virtual std::optional<CommandResult> run(const std::vector<std::string>& argv) = 0;

    virtual ConfigStatusLines read_config_status() = 0;
    // Devices that answer on /dev/i2c-<bus>, or nothing if the bus cannot be
    // opened.
    virtual std::optional<std::vector<I2cDevice>> scan_i2c(int bus) = 0;
    // Loads an image into the FPGA through passive serial. Only used with
    // --identify.
    virtual ConfigResult configure_fpga(std::span<const std::uint8_t> image) = 0;
};

}  // namespace radioberry::probe
