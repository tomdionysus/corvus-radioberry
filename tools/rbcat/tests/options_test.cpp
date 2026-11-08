// SPDX-License-Identifier: GPL-2.0-or-later
#include "options.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace radioberry::cat;

namespace {

ParseResult parse(std::vector<std::string> args) { return parse_options(args); }

Options ok(std::vector<std::string> args) {
    const ParseResult r = parse(std::move(args));
    REQUIRE(std::holds_alternative<Options>(r));
    return std::get<Options>(r);
}

std::string error(std::vector<std::string> args) {
    const ParseResult r = parse(std::move(args));
    REQUIRE(std::holds_alternative<UsageError>(r));
    return std::get<UsageError>(r).message;
}

}  // namespace

TEST_CASE("defaults", "[options]") {
    const Options o = ok({});
    CHECK(o.device == "/dev/radioberry");
    CHECK(o.rate_ksps == 48);
    CHECK(o.rate_code == 0);
    CHECK(o.receivers == 1);
    CHECK(o.frequencies_hz == std::vector<std::uint32_t>{14074000});
    CHECK(o.lna_gain_db == 20);
    CHECK(o.output == Output::kNone);
    CHECK(o.seconds == 0);
    CHECK(o.settle_seconds == 0.5);
    CHECK(o.interval_seconds == 0);
    CHECK(o.format == ReportFormat::kText);
}

TEST_CASE("every option", "[options]") {
    const Options o =
        ok({"--rate", "384", "--receivers", "3", "--freq", "7074000,10136000,14074000", "--lna",
            "-12", "--output", "iq16", "--seconds", "600", "--settle", "1.5", "--interval", "10",
            "--device", "/dev/other", "--yaml"});
    CHECK(o.rate_ksps == 384);
    CHECK(o.rate_code == 3);
    CHECK(o.receivers == 3);
    CHECK(o.frequencies_hz == std::vector<std::uint32_t>{7074000, 10136000, 14074000});
    CHECK(o.lna_gain_db == -12);
    CHECK(o.output == Output::kIq16);
    CHECK(o.seconds == 600);
    CHECK(o.settle_seconds == 1.5);
    CHECK(o.interval_seconds == 10);
    CHECK(o.device == "/dev/other");
    CHECK(o.format == ReportFormat::kYaml);

    CHECK(ok({"--json"}).format == ReportFormat::kJson);
    CHECK(ok({"--output", "iq24"}).output == Output::kIq24);
    CHECK(ok({"--output", "iq24", "--output", "none"}).output == Output::kNone);
    CHECK(ok({"--rate", "96"}).rate_code == 1);
    CHECK(ok({"--rate", "192"}).rate_code == 2);
}

TEST_CASE("one frequency applies to every receiver", "[options]") {
    CHECK(ok({"--receivers", "4", "--freq", "3573000"}).frequencies_hz ==
          std::vector<std::uint32_t>(4, 3573000));
    // Order does not matter.
    CHECK(ok({"--freq", "1,2", "--receivers", "2"}).frequencies_hz ==
          std::vector<std::uint32_t>{1, 2});
    CHECK(ok({"--freq", "38400000"}).frequencies_hz.front() == 38400000);
}

TEST_CASE("usage errors", "[options]") {
    CHECK(error({"--bogus"}) == "unknown option --bogus");
    CHECK(error({"--rate", "44"}).find("--rate needs 48, 96") == 0);
    CHECK(error({"--rate"}).find("--rate needs") == 0);
    CHECK(error({"--receivers", "0"}).find("--receivers needs") == 0);
    CHECK(error({"--receivers", "13"}).find("--receivers needs") == 0);
    CHECK(error({"--receivers", "two"}).find("--receivers needs") == 0);
    CHECK(error({"--freq", "38400001"}).find("--freq needs") == 0);
    CHECK(error({"--freq", "1,,2"}).find("--freq needs") == 0);
    CHECK(error({"--freq", ""}).find("--freq needs") == 0);
    CHECK(error({"--freq", "-5"}).find("--freq needs") == 0);
    CHECK(error({"--freq", "1,2"}) == "--freq gives 2 frequencies for 1 receivers");
    CHECK(error({"--lna", "49"}).find("--lna needs") == 0);
    CHECK(error({"--lna", "-13"}).find("--lna needs") == 0);
    CHECK(error({"--lna"}).find("--lna needs") == 0);
    CHECK(error({"--output", "wav"}).find("--output needs") == 0);
    CHECK(error({"--output"}).find("--output needs") == 0);
    CHECK(error({"--seconds", "-1"}).find("--seconds needs") == 0);
    CHECK(error({"--seconds", "inf"}).find("--seconds needs") == 0);
    CHECK(error({"--settle", "x"}).find("--settle needs") == 0);
    CHECK(error({"--interval"}).find("--interval needs") == 0);
    CHECK(error({"--device"}).find("--device needs") == 0);
}

TEST_CASE("help and version", "[options]") {
    CHECK(std::holds_alternative<ShowHelp>(parse({"--help"})));
    CHECK(std::holds_alternative<ShowHelp>(parse({"--rate", "48", "-h"})));
    CHECK(std::holds_alternative<ShowVersion>(parse({"--version"})));
}

TEST_CASE("output names", "[options]") {
    CHECK(to_string(Output::kNone) == "none");
    CHECK(to_string(Output::kIq24) == "iq24");
    CHECK(to_string(Output::kIq16) == "iq16");
}
