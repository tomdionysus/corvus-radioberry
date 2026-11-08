// SPDX-License-Identifier: GPL-2.0-or-later
#include "options.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace radioberry::daemon;
using namespace std::chrono_literals;

TEST_CASE("radioberryd's defaults", "[options]") {
    const Options o = parse_options({});
    CHECK(o.device == "/dev/radioberry");
    CHECK(o.daemon.address == 0);
    CHECK(o.daemon.port == 1024);
    CHECK_FALSE(o.daemon.mac);
    CHECK(o.daemon.server.client_timeout == 3s);
    CHECK(o.log_level == "info");
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
    CHECK(o.daemon.server.client_timeout == 500ms);
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
}
