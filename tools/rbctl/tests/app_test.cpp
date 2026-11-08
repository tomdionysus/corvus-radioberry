// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <yaml-cpp/yaml.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <vector>

using namespace radioberry;
using namespace radioberry::ctl;

namespace {

// Records commands; answers as a CL025 with gateware 75.3.
class FakeDevice final : public Device {
public:
    radioberry_info info() override {
        radioberry_info i{};
        i.abi_version = RADIOBERRY_ABI_VERSION;
        i.driver_version = 0x000100;
        i.gateware_major = 75;
        i.gateware_minor = 3;
        i.fpga_type = RADIOBERRY_FPGA_CL025;
        i.receivers = 6;
        i.transmitters = 1;
        i.active_receivers = 2;
        i.rx_rate_code = rate_code;
        return i;
    }
    radioberry_stats stats() override {
        radioberry_stats s{};
        s.rx_words = 100;
        s.rx_discarded_words = 24;
        s.commands = commands.size();
        s.flags = RADIOBERRY_STATS_RX_OVERRUN;
        return s;
    }
    Reply command(const Command& c) override {
        if (fail_command) {
            throw std::system_error(EIO, std::generic_category(), "/dev/radioberry: command");
        }
        commands.push_back(c);
        Reply r;
        r.external_cw_key = true;
        r.receivers = 6;
        r.transmitters = 1;
        r.fpga_type = 2;
        r.version_major = 75;
        r.version_minor = 3;
        return r;
    }
    std::size_t read(std::span<std::uint8_t>) override { return 0; }
    std::size_t write(std::span<const std::uint8_t> b) override { return b.size(); }

    std::vector<Command> commands;
    std::uint8_t rate_code = 1;
    bool fail_command = false;
};

struct Run {
    int code = 0;
    std::string out;
    std::string err;
    std::string opened;
};

Run run(std::vector<std::string> args, FakeDevice* prepared = nullptr, bool fail_open = false) {
    Run r;
    std::ostringstream out;
    std::ostringstream err;
    Environment env{
        .open = [&](const std::string& path) -> std::unique_ptr<Device> {
            r.opened = path;
            if (fail_open) {
                throw std::system_error(EBUSY, std::generic_category(), path);
            }
            auto d = std::make_unique<FakeDevice>();
            if (prepared != nullptr) {
                *d = *prepared;
            }
            return d;
        },
        .out = out,
        .err = err,
    };
    r.code = run_app(args, env);
    r.out = out.str();
    r.err = err.str();
    return r;
}

nlohmann::json json(const Run& r) { return nlohmann::json::parse(r.out); }

}  // namespace

TEST_CASE("info and statistics without a command", "[rbctl]") {
    const Run r = run({"--json"});
    CHECK(r.code == kExitOk);
    CHECK(r.opened == "/dev/radioberry");
    const auto j = json(r);
    CHECK_FALSE(j.contains("command"));
    CHECK_FALSE(j.contains("reply"));
    CHECK(j["device"] == "/dev/radioberry");
    CHECK(j["info"]["gateware"] == "75.3");
    CHECK(j["info"]["fpga"] == "10CL025");
    CHECK(j["info"]["driver_version"] == "0.1.0");
    CHECK(j["info"]["active_receivers"] == 2);
    CHECK(j["info"]["rx_rate_ksps"] == 96);
    CHECK(j["statistics"]["rx_words"] == 100);
    CHECK(j["statistics"]["rx_discarded_words"] == 24);
    CHECK(j["statistics"]["commands"] == 0);
    CHECK(j["statistics"]["rx_overrun_flag"] == true);
    CHECK(j["statistics"]["ptt_flag"] == false);
}

TEST_CASE("an unknown rate code reads as null", "[rbctl]") {
    FakeDevice odd;
    odd.rate_code = 7;
    CHECK(json(run({"--json"}, &odd))["info"]["rx_rate_ksps"].is_null());
}

TEST_CASE("a receive-only command is sent and reported", "[rbctl]") {
    const Run r = run({"--json", "0x0a", "0x60"});
    CHECK(r.code == kExitOk);
    const auto j = json(r);
    CHECK(j["command"]["address"] == "0x0a");
    CHECK(j["command"]["data"] == "0x00000060");
    CHECK(j["command"]["run"] == true);
    CHECK(j["command"]["ptt"] == false);
    CHECK(j["command"]["tx_permission"] == false);
    CHECK(j["command"]["cwx"] == false);
    CHECK(j["command"]["bytes"] == "01 14 00 00 00 60");
    CHECK(j["command"]["transmit_hazards"].empty());
    CHECK(j["reply"]["external_cw_key"] == true);
    CHECK(j["reply"]["cw_or_ptt"] == false);
    CHECK(j["reply"]["gateware"] == "75.3");
    CHECK(j["reply"]["fpga"] == "10CL025");
    CHECK(j["reply"]["receivers"] == 6);
    CHECK(j["reply"]["transmitters"] == 1);
    CHECK(j["statistics"]["commands"] == 1);
}

TEST_CASE("decimal numbers, --no-run and the device path", "[rbctl]") {
    const Run r = run({"--yaml", "--no-run", "--device", "/dev/other", "2", "14074000"});
    CHECK(r.code == kExitOk);
    CHECK(r.opened == "/dev/other");
    const YAML::Node y = YAML::Load(r.out);
    CHECK(y["command"]["address"].as<std::string>() == "0x02");
    CHECK(y["command"]["data"].as<std::string>() == "0x00d6c090");
    CHECK(y["command"]["run"].as<bool>() == false);
    CHECK(y["command"]["bytes"].as<std::string>() == "00 04 00 d6 c0 90");
}

TEST_CASE("text output", "[rbctl]") {
    const Run r = run({"0X09", "0"});
    CHECK(r.code == kExitOk);
    CHECK(r.out.find("device\n  /dev/radioberry\n") == 0);
    CHECK(r.out.find("command\n  address           0x09\n") != std::string::npos);
    CHECK(r.out.find("  transmit_hazards  none\n") != std::string::npos);
    CHECK(r.out.find("statistics\n") != std::string::npos);
}

TEST_CASE("commands that can transmit are refused without --allow-tx", "[rbctl]") {
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"--ptt", "0", "0"},
             {"--tx-permit", "0", "0"},
             {"--cwx", "0", "0"},
             {"9", "0x00080000"},  // PA enable
             {"9", "0x10000000"},  // drive
             {"9", "0x00800000"},  // VNA
         }) {
        const Run r = run(args);
        CHECK(r.code == kExitUsage);
        CHECK(r.err.find("rbctl: refusing a command that can transmit (") == 0);
        CHECK(r.err.find("without --allow-tx") != std::string::npos);
        CHECK(r.opened.empty());
    }
    const Run listed = run({"--ptt", "--tx-permit", "9", "0x00080000"});
    CHECK(listed.err ==
          "rbctl: refusing a command that can transmit (PTT, TX permission, PA enable) without "
          "--allow-tx\n");
}

TEST_CASE("with --allow-tx a hazardous command is sent and its hazards reported", "[rbctl]") {
    const Run r = run({"--json", "--allow-tx", "--ptt", "--tx-permit", "--cwx", "1", "7074000"});
    CHECK(r.code == kExitOk);
    const auto j = json(r);
    CHECK(j["command"]["ptt"] == true);
    CHECK(j["command"]["tx_permission"] == true);
    CHECK(j["command"]["cwx"] == true);
    CHECK(j["command"]["bytes"] == "07 03 00 6b f0 d0");
    CHECK(j["command"]["transmit_hazards"] ==
          nlohmann::json::array({"PTT", "TX permission", "CWX"}));
}

TEST_CASE("PTT hang time 31 is always refused", "[rbctl]") {
    for (const bool allow : {false, true}) {
        std::vector<std::string> args = {"0x17", "0x1f14"};
        if (allow) {
            args.insert(args.begin(), "--allow-tx");
        }
        const Run r = run(args);
        CHECK(r.code == kExitUsage);
        CHECK(r.err ==
              "rbctl: refusing PTT hang time 31: it disables the gateware's transmit watchdog\n");
        CHECK(r.opened.empty());
    }
    CHECK(run({"0x17", "0x0c14"}).code == kExitOk);
}

TEST_CASE("usage errors", "[rbctl]") {
    auto error = [](std::vector<std::string> args) {
        const Run r = run(std::move(args));
        CHECK(r.code == kExitUsage);
        CHECK(r.opened.empty());
        return r.err.substr(0, r.err.find('\n'));
    };
    CHECK(error({"--bogus"}) == "rbctl: unknown option --bogus");
    CHECK(error({"--device"}) == "rbctl: --device needs a path");
    CHECK(error({"1"}) == "rbctl: expected ADDRESS and DATA");
    CHECK(error({"1", "2", "3"}) == "rbctl: expected ADDRESS and DATA");
    CHECK(error({"0x40", "0"}) == "rbctl: ADDRESS must be 0 to 0x3f, not 0x40");
    CHECK(error({"x", "0"}) == "rbctl: ADDRESS must be 0 to 0x3f, not x");
    CHECK(error({"0x", "0"}) == "rbctl: ADDRESS must be 0 to 0x3f, not 0x");
    CHECK(error({"1", "0x100000000"}) == "rbctl: DATA must be a 32-bit number, not 0x100000000");
    CHECK(error({"1", "12z"}) == "rbctl: DATA must be a 32-bit number, not 12z");
    CHECK(error({"--ptt"}) ==
          "rbctl: --ptt, --tx-permit, --cwx and --no-run need ADDRESS and DATA");
    CHECK(error({"--no-run"}) ==
          "rbctl: --ptt, --tx-permit, --cwx and --no-run need ADDRESS and DATA");
    // A lone "-" is a positional argument, not an option.
    CHECK(error({"-", "0"}) == "rbctl: ADDRESS must be 0 to 0x3f, not -");
}

TEST_CASE("device errors exit 1", "[rbctl]") {
    Run r = run({}, nullptr, true);
    CHECK(r.code == kExitDeviceError);
    CHECK(r.err.find("rbctl: /dev/radioberry: ") == 0);

    FakeDevice failing;
    failing.fail_command = true;
    r = run({"0", "0"}, &failing);
    CHECK(r.code == kExitDeviceError);
    CHECK(r.err.find("rbctl: /dev/radioberry: command: ") == 0);
    CHECK(r.out.empty());
}

TEST_CASE("help and version", "[rbctl]") {
    Run r = run({"--help"});
    CHECK(r.code == kExitOk);
    CHECK(r.out.find("usage: rbctl") == 0);
    CHECK(run({"-h"}).out.find("usage: rbctl") == 0);
    r = run({"--version"});
    CHECK(r.code == kExitOk);
    CHECK(r.out.find("rbctl ") == 0);
    CHECK(r.opened.empty());
}

TEST_CASE("refusal of options without a command", "[rbctl]") {
    CHECK_FALSE(refusal(Options{}).has_value());
}
