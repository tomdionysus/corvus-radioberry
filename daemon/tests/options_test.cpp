// SPDX-License-Identifier: GPL-2.0-or-later
#include "options.hpp"

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace radioberry::daemon;
using namespace std::chrono_literals;

TEST_CASE("radioberryd's defaults", "[options]") {
    const Options o = parse_options({});
    CHECK(o.device == "/dev/radioberry");
    CHECK(o.daemon.address == 0);
    CHECK(o.daemon.port == 1024);
    CHECK_FALSE(o.daemon.mac);
    CHECK(o.daemon.client_timeout == 3s);
    CHECK(o.log_level == "info");
    CHECK_FALSE(o.daemon.tx.allow_tx);
    CHECK_FALSE(o.daemon.tx.allow_pa);
    CHECK(o.daemon.tx.drive_ceiling == 0);
    CHECK(o.daemon.tx.watchdog == 120s);
    CHECK(o.daemon.device.tx_ring_limit_words == 4800);
    CHECK(o.daemon.frequency_ppm == 0);
    CHECK(o.daemon.iq_convention == p1::IqConvention::Hl2);
    CHECK(o.daemon.p2_rx_gain_db == 14);
    CHECK(o.daemon.realtime);
    CHECK(o.daemon.protocol1);
    CHECK(o.daemon.protocol2);
    CHECK_FALSE(o.help);
    CHECK_FALSE(o.version);
    CHECK(usage().find("--client-timeout-ms") != std::string::npos);
}

TEST_CASE("radioberryd's options", "[options]") {
    const Options o = parse_options({"--device", "/dev/other", "--address", "192.0.2.7", "--port",
                                     "1025", "--mac", "02:00:5e:0a:0B:ff", "--client-timeout-ms",
                                     "500", "--log-level", "debug", "--version", "-h"});
    CHECK(o.device == "/dev/other");
    CHECK(o.daemon.address == 0xc0000207);
    CHECK(o.daemon.port == 1025);
    REQUIRE(o.daemon.mac);
    CHECK(*o.daemon.mac == std::array<std::uint8_t, 6>{0x02, 0x00, 0x5e, 0x0a, 0x0b, 0xff});
    CHECK(o.daemon.client_timeout == 500ms);
    CHECK(o.log_level == "debug");
    CHECK(o.version);
    CHECK(o.help);
    CHECK(parse_options({"--help"}).help);
}

TEST_CASE("radioberryd refuses bad options", "[options]") {
    CHECK_THROWS_AS(parse_options({"--bogus"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--port"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--port", "65536"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--port", "12x"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--address", "300.1.1.1"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--mac", "02:00:5e"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--mac", "02:00:5e:0a:0b:ff:00"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--log-level", "loud"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--client-timeout-ms", "-1"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--drive-ceiling", "256"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--tx-watchdog-s", "0"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--tx-watchdog-s", "3601"}), std::invalid_argument);
}

TEST_CASE("radioberryd's transmit options", "[options]") {
    const Options o = parse_options(
        {"--allow-tx", "--allow-pa", "--drive-ceiling", "64", "--tx-watchdog-s", "30"});
    CHECK(o.daemon.tx.allow_tx);
    CHECK(o.daemon.tx.allow_pa);
    CHECK(o.daemon.tx.drive_ceiling == 64);
    CHECK(o.daemon.tx.watchdog == 30s);
    CHECK(parse_options({"--frequency-ppm", "22.9"}).daemon.frequency_ppm == 22.9);
    CHECK(parse_options({"--frequency-ppm", "-5"}).daemon.frequency_ppm == -5);
    CHECK_THROWS_AS(parse_options({"--frequency-ppm", "201"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--frequency-ppm", "2x"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--frequency-ppm", "nan"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--frequency-ppm", "ppm"}), std::invalid_argument);
    CHECK(parse_options({"--iq-convention", "standard"}).daemon.iq_convention ==
          p1::IqConvention::Standard);
    CHECK(parse_options({"--iq-convention", "hl2"}).daemon.iq_convention == p1::IqConvention::Hl2);
    CHECK_THROWS_AS(parse_options({"--iq-convention", "qi"}), std::invalid_argument);
}

TEST_CASE("radioberryd's Protocol 2 gain", "[options]") {
    CHECK(parse_options({"--p2-rx-gain", "-5"}).daemon.p2_rx_gain_db == -5);
    CHECK(parse_options({"--p2-rx-gain", "48"}).daemon.p2_rx_gain_db == 48);
    CHECK_THROWS_AS(parse_options({"--p2-rx-gain", "49"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--p2-rx-gain", "-13"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--p2-rx-gain", "4.5"}), std::invalid_argument);
    CHECK(usage().find("--p2-rx-gain") != std::string::npos);
    CHECK_FALSE(parse_options({"--no-realtime"}).daemon.realtime);
}

TEST_CASE("radioberryd's true-or-false options take a value or stand alone", "[options]") {
    Options o = parse_options({"--allow-tx", "true", "--allow-pa", "On", "--realtime", "false"});
    CHECK(o.daemon.tx.allow_tx);
    CHECK(o.daemon.tx.allow_pa);
    CHECK_FALSE(o.daemon.realtime);
    o = parse_options({"--allow-tx", "false", "--allow-pa", "no", "--realtime", "yes"});
    CHECK_FALSE(o.daemon.tx.allow_tx);
    CHECK_FALSE(o.daemon.tx.allow_pa);
    CHECK(o.daemon.realtime);
    // Alone, as older command lines give them: true.
    o = parse_options({"--allow-tx", "--allow-pa", "--port", "1025", "--realtime"});
    CHECK(o.daemon.tx.allow_tx);
    CHECK(o.daemon.tx.allow_pa);
    CHECK(o.daemon.realtime);
    CHECK(o.daemon.port == 1025);
    CHECK_THROWS_AS(parse_options({"--allow-tx", "maybe"}), std::invalid_argument);
    CHECK(usage().find("--realtime BOOL") != std::string::npos);
}

TEST_CASE("radioberryd serves the protocols asked for", "[options]") {
    Options o = parse_options({"--protocols", "1"});
    CHECK(o.daemon.protocol1);
    CHECK_FALSE(o.daemon.protocol2);
    o = parse_options({"--protocols", "2"});
    CHECK_FALSE(o.daemon.protocol1);
    CHECK(o.daemon.protocol2);
    o = parse_options({"--protocols", "2, 1"});
    CHECK(o.daemon.protocol1);
    CHECK(o.daemon.protocol2);
    CHECK_THROWS_AS(parse_options({"--protocols", ""}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--protocols", "3"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--protocols", "1,"}), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--protocols"}), std::invalid_argument);
}

namespace {

// A config file with `text`, removed at the end of the scope.
struct ConfigFile {
    explicit ConfigFile(const std::string& text) {
        char name[] = "/tmp/radioberryd-config-XXXXXX";
        const int fd = ::mkstemp(name);
        ::close(fd);
        path = name;
        std::ofstream(path) << text;
    }
    ~ConfigFile() { std::remove(path.c_str()); }
    std::string path;
};

}  // namespace

TEST_CASE("radioberryd reads a config file, the command line winning", "[options]") {
    const ConfigFile f(
        "device: /dev/other\n"
        "port: 1030\n"
        "frequency_ppm: 22.9\n"
        "iq_convention: standard\n"
        "p2_rx_gain: -3\n"
        "realtime: false\n"
        "allow_tx: true\n"
        "allow_pa: false\n"
        "drive_ceiling: 150\n"
        "log_level: debug\n");
    Options o = parse_options({"--config", f.path});
    CHECK(o.device == "/dev/other");
    CHECK(o.daemon.port == 1030);
    CHECK(o.daemon.frequency_ppm == 22.9);
    CHECK(o.daemon.iq_convention == p1::IqConvention::Standard);
    CHECK(o.daemon.p2_rx_gain_db == -3);
    CHECK_FALSE(o.daemon.realtime);
    CHECK(o.daemon.tx.allow_tx);
    CHECK_FALSE(o.daemon.tx.allow_pa);
    CHECK(o.daemon.tx.drive_ceiling == 150);
    CHECK(o.log_level == "debug");
    // The command line overrides the file, wherever --config stands.
    o = parse_options({"--drive-ceiling", "40", "--config", f.path, "--port", "1024"});
    CHECK(o.daemon.tx.drive_ceiling == 40);
    CHECK(o.daemon.port == 1024);
    CHECK(usage().find("--config") != std::string::npos);
    // allow_tx: false in a file is overridden by a bare --allow-tx after it.
    o = parse_options({"--config", ConfigFile("allow_tx: false\n").path, "--allow-tx"});
    CHECK(o.daemon.tx.allow_tx);
}

TEST_CASE("radioberryd reads protocols from a config file", "[options]") {
    Options o = parse_options({"--config", ConfigFile("protocols: 1\n").path});
    CHECK(o.daemon.protocol1);
    CHECK_FALSE(o.daemon.protocol2);
    o = parse_options({"--config", ConfigFile("protocols: 2,1\n").path});
    CHECK(o.daemon.protocol1);
    CHECK(o.daemon.protocol2);
    o = parse_options({"--config", ConfigFile("protocols: [2]\n").path});
    CHECK_FALSE(o.daemon.protocol1);
    CHECK(o.daemon.protocol2);
    CHECK_THROWS_AS(config_args(ConfigFile("protocols: [[1]]\n").path), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--config", ConfigFile("protocols: [3]\n").path}),
                    std::invalid_argument);
}

TEST_CASE("radioberryd refuses a bad config file", "[options]") {
    CHECK(config_args(ConfigFile("").path).empty());
    CHECK(config_args(ConfigFile("# only comments\n").path).empty());
    CHECK_THROWS_AS(parse_options({"--config"}), std::invalid_argument);
    CHECK_THROWS_AS(config_args("/nonexistent/radioberryd.yaml"), std::invalid_argument);
    CHECK_THROWS_AS(config_args(ConfigFile("- a list\n").path), std::invalid_argument);
    CHECK_THROWS_AS(config_args(ConfigFile("port: [1, 2]\n").path), std::invalid_argument);
    CHECK_THROWS_AS(config_args(ConfigFile("colour: blue\n").path), std::invalid_argument);
    CHECK_THROWS_AS(parse_options({"--config", ConfigFile("allow_tx: maybe\n").path}),
                    std::invalid_argument);
    CHECK_THROWS_AS(config_args(ConfigFile("port: [\n").path), std::invalid_argument);
    // Values go through the same checks as on the command line.
    CHECK_THROWS_AS(parse_options({"--config", ConfigFile("port: 70000\n").path}),
                    std::invalid_argument);
}

TEST_CASE("the installed default config: radioberryd's defaults, drive ceiling 150", "[options]") {
    const Options o = parse_options({"--config", RADIOBERRY_DEFAULT_CONFIG});
    const Options d = parse_options({});
    CHECK_FALSE(o.daemon.tx.allow_tx);
    CHECK_FALSE(o.daemon.tx.allow_pa);
    CHECK(o.daemon.tx.drive_ceiling == 150);
    CHECK(o.daemon.tx.watchdog == d.daemon.tx.watchdog);
    CHECK(o.device == d.device);
    CHECK(o.daemon.address == d.daemon.address);
    CHECK(o.daemon.port == d.daemon.port);
    CHECK(o.daemon.mac == d.daemon.mac);
    CHECK(o.daemon.client_timeout == d.daemon.client_timeout);
    CHECK(o.daemon.frequency_ppm == d.daemon.frequency_ppm);
    CHECK(o.daemon.iq_convention == d.daemon.iq_convention);
    CHECK(o.daemon.p2_rx_gain_db == d.daemon.p2_rx_gain_db);
    CHECK(o.daemon.realtime == d.daemon.realtime);
    CHECK(o.daemon.protocol1 == d.daemon.protocol1);
    CHECK(o.daemon.protocol2 == d.daemon.protocol2);
    CHECK(o.log_level == d.log_level);
}
