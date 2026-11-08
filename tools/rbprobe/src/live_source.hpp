// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "source.hpp"

namespace radioberry::probe {

// Reads the running system. Hardware glue; exercised on the development Pi
// rather than by unit tests.
class LiveSource final : public Source {
public:
    std::optional<std::string> read_file(const std::string& path) override;
    bool exists(const std::string& path) override;
    std::vector<std::string> list_dir(const std::string& path) override;
    std::optional<std::string> read_link(const std::string& path) override;
    std::optional<CommandResult> run(const std::vector<std::string>& argv) override;
    ConfigStatusLines read_config_status() override;
    std::optional<std::vector<I2cDevice>> scan_i2c(int bus) override;
    ConfigResult configure_fpga(std::span<const std::uint8_t> image) override;
};

}  // namespace radioberry::probe
