// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.hpp"

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace radioberry::soapy;

namespace {

std::string write_config(const std::string& text) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("rb-soapy-config-" + std::to_string(::getpid()) + ".yaml");
    std::ofstream(path) << text;
    return path.string();
}

}  // namespace

TEST_CASE("no config file: receive only") {
    const Settings s = load_settings("/nonexistent/config.yaml");
    CHECK_FALSE(s.from_file);
    CHECK(s.path == "/nonexistent/config.yaml");
    CHECK(s.device == "/dev/radioberry");
    CHECK(s.frequency_ppm == 0);
    CHECK(s.lna_gain_db == 14);
    CHECK_FALSE(s.tx.allow_tx);
    CHECK_FALSE(s.tx.allow_pa);
}

TEST_CASE("the installed config file") {
    const Settings s = load_settings(RADIOBERRY_DEFAULT_CONFIG);
    CHECK(s.from_file);
    CHECK(s.frequency_ppm == 22.9);
    CHECK_FALSE(s.tx.allow_tx);
    CHECK(s.tx.drive_ceiling == 150);
    CHECK(s.tx.watchdog == std::chrono::seconds(120));
}

TEST_CASE("transmit keys as radioberryd reads them") {
    const std::string path = write_config(
        "device: /dev/other\nallow_tx: true\nallow_pa: true\ndrive_ceiling: 100\n"
        "tx_watchdog_s: 30\np2_rx_gain: 20\nfrequency_ppm: -1.5\n");
    const Settings s = load_settings(path);
    std::filesystem::remove(path);
    CHECK(s.device == "/dev/other");
    CHECK(s.tx.allow_tx);
    CHECK(s.tx.allow_pa);
    CHECK(s.tx.drive_ceiling == 100);
    CHECK(s.tx.watchdog == std::chrono::seconds(30));
    CHECK(s.lna_gain_db == 20);
    CHECK(s.frequency_ppm == -1.5);
}

TEST_CASE("a config file radioberryd would refuse is refused") {
    const std::string path = write_config("allow_tx: perhaps\n");
    CHECK_THROWS_AS(load_settings(path), std::invalid_argument);
    std::filesystem::remove(path);
}
