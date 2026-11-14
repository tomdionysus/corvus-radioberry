// SPDX-License-Identifier: GPL-2.0-or-later
//
// Hardware glue: drives the FPGA configuration lines with libgpiod 2. Not
// unit tested; exercised on the Pi by `rbprobe --identify`.
#include <filesystem>
#include <gpiod.hpp>
#include <stdexcept>
#include <thread>

#include "radioberry/board.hpp"
#include "radioberry/fpga_config.hpp"

namespace radioberry {
namespace {

using gpiod::line::direction;
using gpiod::line::value;

value to_value(bool high) { return high ? value::ACTIVE : value::INACTIVE; }

std::filesystem::path header_chip_or_throw() {
    auto path = find_header_gpio_chip();
    if (!path) {
        throw std::runtime_error("no pinctrl-rp1 or pinctrl-bcm2711 GPIO chip found");
    }
    return *path;
}

class GpiodConfigPins final : public ConfigPins {
public:
    GpiodConfigPins()
        : chip_(header_chip_or_throw()),
          request_(
              chip_.prepare_request()
                  .set_consumer("radioberry-config")
                  .add_line_settings(
                      gpiod::line::offsets{ConfigLines::kConfDone, ConfigLines::kNStatus},
                      gpiod::line_settings().set_direction(direction::INPUT))
                  .add_line_settings(gpiod::line::offsets{ConfigLines::kData, ConfigLines::kDclk},
                                     gpiod::line_settings()
                                         .set_direction(direction::OUTPUT)
                                         .set_output_value(value::INACTIVE))
                  .add_line_settings(gpiod::line::offsets{ConfigLines::kNConfig},
                                     gpiod::line_settings()
                                         .set_direction(direction::OUTPUT)
                                         .set_output_value(value::ACTIVE))
                  .do_request()) {}

    void set_nconfig(bool high) override {
        request_.set_value(ConfigLines::kNConfig, to_value(high));
    }
    void set_data(bool high) override { request_.set_value(ConfigLines::kData, to_value(high)); }
    void set_dclk(bool high) override { request_.set_value(ConfigLines::kDclk, to_value(high)); }
    bool nstatus() override { return request_.get_value(ConfigLines::kNStatus) == value::ACTIVE; }
    bool conf_done() override {
        return request_.get_value(ConfigLines::kConfDone) == value::ACTIVE;
    }
    void delay(std::chrono::microseconds duration) override {
        std::this_thread::sleep_for(duration);
    }

private:
    gpiod::chip chip_;
    gpiod::line_request request_;
};

}  // namespace

std::optional<std::filesystem::path> find_header_gpio_chip() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("gpiochip", 0) != 0 || entry.is_symlink(ec)) {
            continue;
        }
        try {
            gpiod::chip chip(entry.path());
            if (is_header_gpio_chip(chip.get_info().label())) {
                return entry.path();
            }
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): not a usable chip
        }
    }
    return std::nullopt;
}

std::unique_ptr<ConfigPins> open_gpiod_config_pins() {
    try {
        return std::make_unique<GpiodConfigPins>();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("cannot request FPGA configuration lines: ") +
                                 e.what());
    }
}

}  // namespace radioberry
