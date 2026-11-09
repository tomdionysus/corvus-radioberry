// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <map>
#include <set>

#include "radioberry/gateware_catalog.hpp"
#include "source.hpp"

namespace radioberry::probe::test {

// An in-memory machine. Directories are implied by the files, links and
// explicit dirs below them.
class FakeSource final : public Source {
public:
    std::map<std::string, std::string> files;
    std::map<std::string, std::string> links;
    std::set<std::string> dirs;
    std::map<std::vector<std::string>, CommandResult> commands;
    ConfigStatusLines config_lines;
    std::optional<std::vector<I2cDevice>> i2c;
    std::function<ConfigResult(std::span<const std::uint8_t>)> configure;
    std::vector<std::vector<std::string>> commands_run;
    int configure_calls = 0;

    std::optional<std::string> read_file(const std::string& path) override;
    bool exists(const std::string& path) override;
    std::vector<std::string> list_dir(const std::string& path) override;
    std::optional<std::string> read_link(const std::string& path) override;
    std::optional<CommandResult> run(const std::vector<std::string>& argv) override;
    ConfigStatusLines read_config_status() override;
    std::optional<std::vector<I2cDevice>> scan_i2c(int bus) override;
    ConfigResult configure_fpga(std::span<const std::uint8_t> image) override;
};

// Gateware stand-ins: short contents with a matching test catalog.
inline const std::string kCl025Image = "fake rpi-5 CL025 gateware";
inline const std::string kCl016Image = "fake rpi-5 CL016 gateware";
inline const std::string kRpi4Image = "fake rpi-4 CL016 gateware";

std::span<const KnownImage> test_catalog();

// A Pi set up for the original RadioBerry software: its overlay and init
// script, the wrong (rpi-4 CL016) image installed, no driver, an empty I2C
// bus and a CL025 FPGA.
FakeSource development_pi();

// A machine where everything is in place and the radio can run.
FakeSource working_pi();

}  // namespace radioberry::probe::test
