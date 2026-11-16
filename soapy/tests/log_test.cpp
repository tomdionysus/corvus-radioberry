// SPDX-License-Identifier: GPL-2.0-or-later
#include "log.hpp"

#include <spdlog/spdlog.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

#include "log_capture.hpp"

using namespace radioberry::soapy;

TEST_CASE("spdlog levels map to SoapySDR's") {
    CHECK(soapy_level(spdlog::level::critical) == SOAPY_SDR_CRITICAL);
    CHECK(soapy_level(spdlog::level::err) == SOAPY_SDR_ERROR);
    CHECK(soapy_level(spdlog::level::warn) == SOAPY_SDR_WARNING);
    CHECK(soapy_level(spdlog::level::info) == SOAPY_SDR_INFO);
    CHECK(soapy_level(spdlog::level::debug) == SOAPY_SDR_DEBUG);
    CHECK(soapy_level(spdlog::level::trace) == SOAPY_SDR_DEBUG);
}

TEST_CASE("the reused code's messages reach SoapySDR's logger") {
    install_logger("ignore me ");
    install_logger("a second call changes nothing ");
    const test::LogCapture capture;
    spdlog::warn("PTT on: drive {}", 3);
    spdlog::info("ignore me owns the radio");
    spdlog::info("a second call changes nothing either");
    spdlog::default_logger()->flush();
    const auto messages = capture.messages();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0] == std::pair<SoapySDRLogLevel, std::string>(SOAPY_SDR_WARNING,
                                                                  "radioberry: PTT on: drive 3"));
    CHECK(messages[1].second == "radioberry: a second call changes nothing either");
}
