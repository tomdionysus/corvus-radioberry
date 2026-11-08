// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <yaml-cpp/yaml.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

#include "fake_device.hpp"

using namespace radioberry;
using namespace radioberry::cat;
using namespace radioberry::cat::test;

namespace {

struct Run {
    int code = 0;
    std::string out;
    std::string err;
    std::string opened;
};

struct Harness {
    FakeClock clock;
    FakeDevice device{clock};
    std::function<void(FakeDevice&)> setup;
    bool terminal = false;
    bool fail_open = false;

    Run run(std::vector<std::string> args) {
        Run r;
        std::ostringstream out;
        std::ostringstream err;
        if (setup) {
            setup(device);
        }
        Environment env{
            .open = [&](const std::string& path) -> std::unique_ptr<Device> {
                r.opened = path;
                if (fail_open) {
                    throw std::system_error(ENOENT, std::generic_category(), path);
                }
                // The app owns what it opens; hand it a forwarding wrapper.
                struct Forward final : Device {
                    explicit Forward(FakeDevice& fake) : d(fake) {}
                    radioberry_info info() override { return d.info(); }
                    radioberry_stats stats() override { return d.stats(); }
                    Reply command(const Command& c) override { return d.command(c); }
                    std::size_t read(std::span<std::uint8_t> b) override { return d.read(b); }
                    std::size_t write(std::span<const std::uint8_t> b) override {
                        return d.write(b);
                    }
                    FakeDevice& d;
                };
                return std::make_unique<Forward>(device);
            },
            .clock = clock,
            .stop = [] { return false; },
            .out = out,
            .err = err,
            .out_is_terminal = terminal,
        };
        r.code = run_app(args, env);
        r.out = out.str();
        r.err = err.str();
        return r;
    }
};

}  // namespace

TEST_CASE("a clean stream reports and exits 0", "[app]") {
    Harness h;
    const Run r = h.run({"--seconds", "2", "--json"});
    CHECK(r.code == kExitClean);
    CHECK(r.opened == "/dev/radioberry");
    CHECK(r.err.empty());
    const auto j = nlohmann::json::parse(r.out);
    CHECK(j["device"]["gateware"] == "75.3");
    CHECK(j["device"]["fpga"] == "10CL025");
    CHECK(j["device"]["driver_version"] == "0.1.2");
    CHECK(j["device"]["abi_version"] == 1);
    CHECK(j["device"]["gateware_receivers"] == 6);
    CHECK(j["settings"]["rate_ksps"] == 48);
    CHECK(j["settings"]["frequencies_hz"] == nlohmann::json::array({14074000}));
    CHECK(j["settings"]["lna_gain_db"] == 20);
    CHECK(j["settings"]["output"] == "none");
    CHECK(j["settings"]["seconds"] == 2.0);
    CHECK(j["stream"]["frame_rate"] == 48000.0);
    CHECK(j["stream"]["expected_frame_rate"] == 48000);
    CHECK(j["stream"]["interrupted"] == false);
    CHECK(j["levels"][0]["receiver"] == 1);
    CHECK(j["levels"][0]["rms_dbfs"] == -6.0);
    CHECK(j["driver"]["rx_resyncs"] == 0);
    CHECK(j["driver"]["rx_period_bytes"] == 288);
    CHECK(j["settling"]["rx_resyncs"] == 0);
    CHECK(j["safety"]["commands_sent"] == 3);
    CHECK(j["safety"]["ptt_set"] == false);
    CHECK(j["safety"]["tx_permission_set"] == false);
    CHECK(j["stream_clean"] == true);
}

TEST_CASE("text and YAML reports", "[app]") {
    Harness h;
    const Run text = h.run({"--seconds", "0.5"});
    CHECK(text.code == kExitClean);
    CHECK(text.out.find("device\n  path ") == 0);
    CHECK(text.out.find("  - receiver 1, rms_dbfs -6, peak_dbfs -6\n") != std::string::npos);
    CHECK(text.out.find("stream_clean\n  yes\n") != std::string::npos);

    Harness y;
    const Run yaml = y.run({"--seconds", "0.5", "--yaml", "--device", "/dev/x"});
    CHECK(yaml.opened == "/dev/x");
    const YAML::Node doc = YAML::Load(yaml.out);
    CHECK(doc["stream_clean"].as<bool>());
    CHECK(doc["stream"]["frame_rate"].as<double>() == 48000.0);
}

TEST_CASE("samples on stdout put the report on stderr", "[app]") {
    Harness h;
    const Run r = h.run({"--seconds", "0.01", "--output", "iq16", "--json"});
    CHECK(r.code == kExitClean);
    CHECK(r.out.size() >= 480 * 4);
    CHECK(r.out.size() % 4 == 0);
    const auto j = nlohmann::json::parse(r.err);
    CHECK(j["stream"]["bytes_written"] == r.out.size());
    CHECK(j["settings"]["output"] == "iq16");
}

TEST_CASE("binary output to a terminal is refused", "[app]") {
    Harness h;
    h.terminal = true;
    const Run r = h.run({"--output", "iq24"});
    CHECK(r.code == kExitUsage);
    CHECK(r.err == "rbcat: refusing to write binary samples to a terminal\n");
    CHECK(r.opened.empty());
    // Reports alone are fine on a terminal.
    CHECK(h.run({"--seconds", "0.01"}).code == kExitClean);
}

TEST_CASE("stream faults exit 1", "[app]") {
    Harness resync;
    resync.setup = [](FakeDevice& d) {
        d.on_read = [](FakeDevice& dev) {
            if (dev.reads == 900) {
                dev.stats_.rx_resyncs += 1;
            }
        };
    };
    Run r = resync.run({"--seconds", "1", "--json"});
    CHECK(r.code == kExitStreamError);
    auto j = nlohmann::json::parse(r.out);
    CHECK(j["driver"]["rx_resyncs"] == 1);
    CHECK(j["stream_clean"] == false);

    Harness slow;
    slow.device.rate_scale = 0.98;
    r = slow.run({"--seconds", "2", "--json"});
    CHECK(r.code == kExitStreamError);

    Harness short_slow;
    short_slow.device.rate_scale = 0.98;
    CHECK(short_slow.run({"--seconds", "0.5"}).code == kExitClean);
}

TEST_CASE("device failures exit 1 with a message", "[app]") {
    Harness missing;
    missing.fail_open = true;
    Run r = missing.run({});
    CHECK(r.code == kExitStreamError);
    CHECK(r.err.find("rbcat: /dev/radioberry: ") == 0);

    Harness small;
    small.device.info_.receivers = 4;
    r = small.run({"--receivers", "6"});
    CHECK(r.code == kExitStreamError);
    CHECK(r.err == "rbcat: the gateware has 4 receivers; 6 requested\n");
}

TEST_CASE("usage, help and version", "[app]") {
    Harness h;
    Run r = h.run({"--bogus"});
    CHECK(r.code == kExitUsage);
    CHECK(r.err.find("rbcat: unknown option --bogus\nusage: rbcat") == 0);
    r = h.run({"--help"});
    CHECK(r.code == kExitClean);
    CHECK(r.out.find("usage: rbcat") == 0);
    r = h.run({"--version"});
    CHECK(r.code == kExitClean);
    CHECK(r.out.find("rbcat ") == 0);
    CHECK(h.device.commands.empty());
}

TEST_CASE("report of a stream stopped while settling", "[app]") {
    Options o;
    StreamResult result;
    result.interrupted = true;
    const Node report = build_report(o, result);
    CHECK(report.at("stream.frame_rate")->is_null());
    CHECK(report.at("settings.seconds")->is_null());
    CHECK(report.at("stream_clean")->as_bool() == false);
    CHECK_FALSE(stream_clean(o, result));
}

TEST_CASE("tx zeros report and verdict", "[app][tx]") {
    Harness h;
    Run r = h.run({"--seconds", "2", "--tx-zeros", "--json"});
    CHECK(r.code == kExitClean);
    auto j = nlohmann::json::parse(r.out);
    CHECK(j["settings"]["tx_zeros"] == true);
    CHECK(j["tx"]["tx_word_rate"].get<double>() > 47900);
    CHECK(j["tx"]["tx_word_rate"].get<double>() < 48100);
    CHECK(j["tx"]["expected_tx_word_rate"] == 48000);
    CHECK(j["tx"]["tx_underruns"] == 0);
    CHECK(j["tx"]["tx_ring_target_words"] == 2400);
    CHECK(j["tx"]["tx_ring_min_words"].get<int>() > 0);
    CHECK(j["tx"]["bytes_written_to_device"].get<long>() > 0);
    CHECK(j["safety"]["driver_ptt_flag"] == false);
    CHECK(j["safety"]["gateware_reported_tx"] == false);

    Harness plain;
    r = plain.run({"--seconds", "0.1", "--json"});
    j = nlohmann::json::parse(r.out);
    CHECK(j["settings"]["tx_zeros"] == false);
    CHECK_FALSE(j.contains("tx"));
}

TEST_CASE("tx faults exit 1", "[app][tx]") {
    Harness slow;
    slow.device.tx_rate_scale = 0.9;
    CHECK(slow.run({"--seconds", "2", "--tx-zeros"}).code == kExitStreamError);

    Harness underrun;
    underrun.setup = [](FakeDevice& d) {
        d.on_read = [](FakeDevice& dev) {
            if (dev.reads == 900) {
                dev.stats_.tx_underruns += 1;
            }
        };
    };
    CHECK(underrun.run({"--seconds", "1", "--tx-zeros"}).code == kExitStreamError);

    // A device that does not drain: no words, but the ring never empties.
    Harness stuck;
    stuck.device.tx_rate_scale = 0;
    CHECK(stuck.run({"--seconds", "2", "--tx-zeros"}).code == kExitStreamError);
    Harness stuck_short;
    stuck_short.device.tx_rate_scale = 0;
    CHECK(stuck_short.run({"--seconds", "0.5", "--tx-zeros"}).code == kExitClean);

    // A ring found empty: the device drains faster than rbcat tops up.
    Harness starved;
    starved.device.tx_rate_scale = 60;
    CHECK(starved.run({"--seconds", "0.5", "--tx-zeros"}).code == kExitStreamError);
}

TEST_CASE("tx verdict pieces", "[app][tx]") {
    Options o;
    StreamResult r;
    CHECK(tx_clean(o, r));
    o.tx_zeros = true;
    CHECK_FALSE(tx_clean(o, r));  // ring never sampled
    r.tx_ring_min_words = 10;
    CHECK(tx_clean(o, r));  // under a second: rate not judged
    r.tx_ring_min_words = 0;
    CHECK_FALSE(tx_clean(o, r));
}
