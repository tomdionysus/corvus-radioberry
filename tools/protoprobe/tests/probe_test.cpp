// SPDX-License-Identifier: GPL-2.0-or-later
#include "probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <sstream>
#include <stdexcept>

#include "fake_radio.hpp"

using namespace radioberry;
using namespace radioberry::proto;
using radioberry::proto::test::FakeRadio;

namespace {

Options parse(Protocol protocol, std::vector<std::string> args) {
    return std::get<Options>(parse_options(protocol, args));
}

bool usage_error(Protocol protocol, std::vector<std::string> args) {
    return std::holds_alternative<UsageError>(parse_options(protocol, args));
}

Node run_probe(Protocol protocol, FakeRadio& radio, const Options& options) {
    const Clock clock = radio.clock();
    const std::unique_ptr<Client> client =
        protocol == Protocol::kOne ? make_p1_client(radio) : make_p2_client(radio, clock);
    return probe(protocol, options, *client, clock);
}

std::vector<std::string> findings(const Node& report) {
    std::vector<std::string> out;
    for (const Node& f : report.find("findings")->items()) {
        out.push_back(f.as_string());
    }
    return out;
}

}  // namespace

TEST_CASE("the probes parse their options", "[protoprobe]") {
    const Options d = parse(Protocol::kOne, {});
    CHECK(d.host == "127.0.0.1");
    CHECK(d.port == 1024);
    CHECK(d.stream.rate_ksps == 48);
    CHECK(d.stream.receivers == 1);
    CHECK(d.stream.frequency_hz == 7074000);
    CHECK_FALSE(d.stream.lna_db);
    CHECK_FALSE(d.mox);
    CHECK_FALSE(d.discover_only);

    const Options o =
        parse(Protocol::kOne, {"192.0.2.1",   "--port",    "1100",   "--rate",          "384",
                               "--receivers", "3",         "--freq", "14074000",        "--lna",
                               "-5",          "--seconds", "1.5",    "--settle",        "0",
                               "--tolerance", "5",         "--mox",  "--discover-only", "--json"});
    CHECK(o.host == "192.0.2.1");
    CHECK(o.port == 1100);
    CHECK(o.stream.rate_ksps == 384);
    CHECK(o.stream.receivers == 3);
    CHECK(o.stream.frequency_hz == 14074000);
    CHECK(o.stream.lna_db == -5);
    CHECK(o.seconds == 1.5);
    CHECK(o.settle == 0);
    CHECK(o.tolerance_percent == 5);
    CHECK(o.mox);
    CHECK(o.discover_only);
    CHECK(o.format == ReportFormat::kJson);
    CHECK(parse(Protocol::kTwo, {"--receivers", "8", "--yaml"}).stream.receivers == 8);
    CHECK(parse(Protocol::kTwo, {"--yaml"}).format == ReportFormat::kYaml);

    CHECK(std::holds_alternative<ShowHelp>(
        parse_options(Protocol::kOne, std::vector<std::string>{"-h"})));
    CHECK(std::holds_alternative<ShowVersion>(
        parse_options(Protocol::kTwo, std::vector<std::string>{"--version"})));
    CHECK(usage_error(Protocol::kOne, {"--rate", "50"}));
    CHECK(usage_error(Protocol::kOne, {"--rate"}));
    CHECK(usage_error(Protocol::kOne, {"--receivers", "8"}));
    CHECK(usage_error(Protocol::kTwo, {"--receivers", "0"}));
    CHECK(usage_error(Protocol::kTwo, {"--lna", "10"}));  // Protocol 1 only
    CHECK(usage_error(Protocol::kOne, {"--lna", "49"}));
    CHECK(usage_error(Protocol::kOne, {"--port", "0"}));
    CHECK(usage_error(Protocol::kOne, {"--port", "65530"}));
    CHECK(usage_error(Protocol::kOne, {"--freq", "70000000"}));
    CHECK(usage_error(Protocol::kOne, {"--seconds", "0"}));
    CHECK(usage_error(Protocol::kOne, {"--settle", "-1"}));
    CHECK(usage_error(Protocol::kOne, {"--tolerance", "x"}));
    CHECK(usage_error(Protocol::kOne, {"--bogus"}));
    CHECK(usage_error(Protocol::kOne, {"a", "b"}));
}

TEST_CASE("p1probe discovers and measures a Protocol 1 stream", "[protoprobe]") {
    FakeRadio radio(false);
    Options o = parse(Protocol::kOne, {"--receivers", "2", "--rate", "96", "--lna", "20"});
    const Node report = run_probe(Protocol::kOne, radio, o);
    CHECK(findings(report).empty());
    CHECK(report.find("pass")->as_bool());
    CHECK(report.at("radio.gateware")->as_string() == "75.3");
    CHECK(report.at("radio.mac")->as_string() == "02:00:00:00:00:09");
    CHECK(report.at("radio.receivers")->as_int() == 6);
    // 96 ksps, two receivers: 36 samples per half, 72 per packet.
    CHECK(report.at("stream.expected_per_second")->as_double() == Catch::Approx(1333.3));
    CHECK(report.at("stream.packets_per_second")->as_double() ==
          Catch::Approx(1333.3).epsilon(0.01));
    CHECK(report.at("stream.sequence_breaks")->as_int() == 0);
    const auto& levels = report.at("stream.levels")->items();
    REQUIRE(levels.size() == 2);
    // I = 2^20, Q = -2^20: RMS sqrt(2) * 2^20 against 2^23.
    CHECK(levels[1].find("rms_dbfs")->as_double() == Catch::Approx(-15.1));
    CHECK(levels[1].find("peak_dbfs")->as_double() == Catch::Approx(-18.1));
    // The configuration the radio was given; drive 0, no MOX.
    CHECK(radio.p1_commands[0x00] == (1u << 24 | 1u << 3 | 1u << 2));
    CHECK(radio.p1_commands[0x01] == 7074000);
    CHECK(radio.p1_commands[0x02] == 7074000);
    CHECK(radio.p1_commands[0x03] == 7074000);
    CHECK(radio.p1_commands[0x09] == 0);
    CHECK(radio.p1_commands[0x0a] == (0x40u | 32));
    CHECK_FALSE(radio.mox_seen);
    CHECK_FALSE(radio.streaming);  // stopped at the end
    // Host packets at 48 ksps of TX samples, 126 each: about 381 a second.
    CHECK(report.at("transmit.host_packets")->as_int() > 700);
    CHECK(report.at("transmit.host_packets")->as_int() < 830);
}

TEST_CASE("p1probe holds MOX at drive 0 when asked", "[protoprobe]") {
    FakeRadio radio(false);
    radio.report_ptt = true;
    const Node report = run_probe(Protocol::kOne, radio, parse(Protocol::kOne, {"--mox"}));
    CHECK(report.find("pass")->as_bool());
    CHECK(radio.mox_seen);
    CHECK_FALSE(radio.mox);  // released before the stop
    CHECK(radio.p1_commands[0x09] == 0);
    CHECK(report.at("transmit.mox")->as_bool());
    CHECK(report.at("transmit.ptt_reported_packets")->as_int() > 0);
}

TEST_CASE("p1probe reports a wrong rate, lost packets and silence", "[protoprobe]") {
    FakeRadio slow(false);
    slow.rate_scale = 0.9;
    auto f = findings(run_probe(Protocol::kOne, slow, parse(Protocol::kOne, {})));
    REQUIRE(f.size() == 1);
    CHECK(f[0].starts_with("packet rate"));
    // A wider tolerance passes it.
    FakeRadio slow2(false);
    slow2.rate_scale = 0.9;
    CHECK(findings(run_probe(Protocol::kOne, slow2, parse(Protocol::kOne, {"--tolerance", "15"})))
              .empty());

    FakeRadio lossy(false);
    lossy.skip_every = 100;
    f = findings(run_probe(Protocol::kOne, lossy, parse(Protocol::kOne, {"--tolerance", "5"})));
    REQUIRE(f.size() == 1);
    CHECK(f[0].find("sequence breaks") != std::string::npos);

    FakeRadio silent(false);
    silent.answer = false;
    const Node report = run_probe(Protocol::kOne, silent, parse(Protocol::kOne, {}));
    CHECK_FALSE(report.find("pass")->as_bool());
    CHECK(findings(report) == std::vector<std::string>{"no discovery reply from 127.0.0.1"});
    CHECK(report.find("stream") == nullptr);
}

TEST_CASE("the probes stop at discovery when asked or when they must", "[protoprobe]") {
    FakeRadio radio(false);
    Node report = run_probe(Protocol::kOne, radio, parse(Protocol::kOne, {"--discover-only"}));
    CHECK(report.find("pass")->as_bool());
    CHECK(report.find("stream") == nullptr);
    CHECK_FALSE(radio.streaming);
    CHECK(radio.host_packets == 0);

    FakeRadio busy(true);
    busy.busy = true;
    report = run_probe(Protocol::kTwo, busy, parse(Protocol::kTwo, {}));
    CHECK(findings(report) == std::vector<std::string>{"the radio is streaming to another client"});
    CHECK(report.at("radio.busy")->as_bool());

    FakeRadio small(true);
    small.receivers = 4;
    report = run_probe(Protocol::kTwo, small, parse(Protocol::kTwo, {"--receivers", "6"}));
    CHECK(findings(report) == std::vector<std::string>{"the radio has 4 receivers, not 6"});
}

TEST_CASE("p2probe discovers and measures a Protocol 2 stream", "[protoprobe]") {
    FakeRadio radio(true);
    const Node report = run_probe(Protocol::kTwo, radio,
                                  parse(Protocol::kTwo, {"--receivers", "3", "--rate", "192"}));
    CHECK(findings(report).empty());
    CHECK(report.at("radio.gateware")->as_string() == "75");
    CHECK(report.at("radio.protocol_version")->as_int() == 38);
    CHECK(report.at("radio.board")->as_int() == 6);
    // Three DDCs at 192 ksps, 238 samples a packet.
    CHECK(report.at("stream.expected_per_second")->as_double() == Catch::Approx(2420.2));
    CHECK(report.at("stream.packets_per_second")->as_double() ==
          Catch::Approx(2420.2).epsilon(0.01));
    CHECK(report.at("stream.levels")->items().size() == 3);
    CHECK(radio.active == 3);
    CHECK(radio.rate_ksps == 192);
    CHECK_FALSE(radio.mox_seen);
    CHECK(radio.tx_iq_packets == 0);  // none without MOX, as piHPSDR
    CHECK_FALSE(radio.streaming);
    // High Priority went out at least every 100 ms.
    std::size_t high_priority = 0;
    for (const Datagram& d : radio.sent) {
        high_priority += d.offset == 3 ? 1 : 0;
    }
    CHECK(high_priority >= 22);
}

TEST_CASE("p2probe holds MOX with zero TX I/Q clocked by Mic/Line", "[protoprobe]") {
    FakeRadio radio(true);
    radio.report_ptt = true;
    const Node report = run_probe(Protocol::kTwo, radio, parse(Protocol::kTwo, {"--mox"}));
    CHECK(findings(report).empty());
    CHECK(radio.mox_seen);
    CHECK_FALSE(radio.mox);
    CHECK_FALSE(radio.tx_iq_gap);
    // 192 ksps of TX I/Q in 240-sample packets for 2 s: 1600, give or take
    // the Mic/Line packets the last poll takes past the end.
    CHECK(radio.tx_iq_packets >= 1590);
    CHECK(radio.tx_iq_packets <= 1610);
    CHECK(report.at("transmit.tx_iq_packets")->as_int() >= 1590);
    CHECK(report.at("transmit.ptt_reported_packets")->as_int() > 0);
}

TEST_CASE("p2probe reports lost DDC packets", "[protoprobe]") {
    FakeRadio radio(true);
    radio.skip_every = 50;
    const auto f = findings(run_probe(
        Protocol::kTwo, radio, parse(Protocol::kTwo, {"--receivers", "2", "--tolerance", "5"})));
    REQUIRE(f.size() == 1);
    CHECK(f[0].find("sequence breaks") != std::string::npos);
}

TEST_CASE("the probe apps render, and report failures by exit status", "[protoprobe]") {
    FakeRadio radio(false);
    std::ostringstream out;
    std::ostringstream err;
    Environment env{
        .open = [&](const std::string&, std::uint16_t) -> std::unique_ptr<Transport> {
            struct Borrowed final : Transport {
                explicit Borrowed(FakeRadio& r) : radio(r) {}
                void send(unsigned offset, std::span<const std::uint8_t> b) override {
                    radio.send(offset, b);
                }
                std::optional<Datagram> receive(std::chrono::milliseconds t) override {
                    return radio.receive(t);
                }
                FakeRadio& radio;
            };
            return std::make_unique<Borrowed>(radio);
        },
        .now = radio.clock(),
        .out = out,
        .err = err,
    };
    const auto run = [&](Protocol p, std::vector<std::string> args) {
        out.str("");
        err.str("");
        return run_app(p, args, env);
    };
    CHECK(run(Protocol::kOne, {"--seconds", "0.5"}) == kExitOk);
    CHECK(out.str().find("packets_per_second") != std::string::npos);
    CHECK(run(Protocol::kOne, {"--seconds", "0.5", "--json"}) == kExitOk);
    CHECK(out.str().find("\"pass\": true") != std::string::npos);
    CHECK(run(Protocol::kOne, {"--discover-only", "--yaml"}) == kExitOk);
    CHECK(out.str().find("pass: true") != std::string::npos);

    radio.answer = false;
    CHECK(run(Protocol::kOne, {}) == kExitFailed);
    CHECK(out.str().find("no discovery reply") != std::string::npos);

    CHECK(run(Protocol::kOne, {"--help"}) == kExitOk);
    CHECK(out.str() == usage(Protocol::kOne));
    CHECK(out.str().find("--lna") != std::string::npos);
    CHECK(usage(Protocol::kTwo).find("--lna") == std::string::npos);
    CHECK(run(Protocol::kTwo, {"--version"}) == kExitOk);
    CHECK(out.str().starts_with("p2probe "));
    CHECK(run(Protocol::kTwo, {"--bogus"}) == kExitUsage);
    CHECK(err.str().starts_with("p2probe: unknown option --bogus"));

    Environment broken{
        .open = [](const std::string&, std::uint16_t) -> std::unique_ptr<Transport> {
            throw std::runtime_error("not an IPv4 address: radio");
        },
        .now = radio.clock(),
        .out = out,
        .err = err,
    };
    err.str("");
    CHECK(run_app(Protocol::kOne, std::vector<std::string>{"radio"}, broken) == kExitFailed);
    CHECK(err.str() == "p1probe: not an IPv4 address: radio\n");
}

TEST_CASE("the clients refuse configurations they cannot send", "[protoprobe]") {
    FakeRadio one(false);
    const auto p1 = make_p1_client(one);
    StreamConfig c;
    c.rate_ksps = 50;
    CHECK_THROWS_AS(p1->start(c), std::runtime_error);
    c.rate_ksps = 48;
    c.receivers = 0;
    CHECK_THROWS_AS(p1->start(c), std::runtime_error);
    c.receivers = 1;
    c.lna_db = 60;
    CHECK_THROWS_AS(p1->start(c), std::runtime_error);

    FakeRadio two(true);
    const auto p2 = make_p2_client(two, two.clock());
    StreamConfig d;
    d.lna_db = 10;
    CHECK_THROWS_AS(p2->start(d), std::runtime_error);
    d.lna_db.reset();
    d.receivers = 9;
    CHECK_THROWS_AS(p2->start(d), std::runtime_error);
    d.receivers = 1;
    d.rate_ksps = 50;
    CHECK_THROWS_AS(p2->start(d), std::runtime_error);
}

TEST_CASE("levels and sequences count as documented", "[protoprobe]") {
    Levels l;
    CHECK_FALSE(std::isfinite(l.rms_dbfs()));
    l.add(8388607, 0);
    CHECK(l.peak_dbfs() == Catch::Approx(0.0).margin(0.001));
    CHECK(l.samples() == 1);

    Sequence s;
    s.add(10);
    s.add(11);
    s.add(14);  // 12 and 13 lost
    s.add(14);  // repeated: a break, nothing lost
    CHECK(s.breaks() == 2);
    CHECK(s.lost() == 2);
    s.reset_counts();
    s.add(15);
    CHECK(s.breaks() == 0);
    // Wrap-around is continuous.
    Sequence w;
    w.add(0xffffffffu);
    w.add(0);
    CHECK(w.breaks() == 0);
}
