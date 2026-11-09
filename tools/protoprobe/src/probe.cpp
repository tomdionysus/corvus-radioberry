// SPDX-License-Identifier: GPL-2.0-or-later
#include "probe.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>

#include "radioberry/render.hpp"
#include "radioberry/version.hpp"

namespace radioberry::proto {
namespace {

using std::chrono::duration;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

const char* name(Protocol protocol) { return protocol == Protocol::kOne ? "p1probe" : "p2probe"; }

template <typename T>
std::optional<T> number(const std::string& text) {
    T value{};
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (text.empty() || ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return value;
}

double seconds_since(const Clock& now, nanoseconds since) {
    return duration<double>(now() - since).count();
}

double round_to(double value, int digits) {
    const double scale = std::pow(10.0, digits);
    return std::round(value * scale) / scale;
}

std::string mac_text(const std::array<std::uint8_t, 6>& mac) {
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    return buf;
}

Node level_db(double db) { return std::isfinite(db) ? Node(round_to(db, 1)) : Node(); }

Node radio_node(const Options& options, const Radio& radio, double discovery_ms) {
    Node n = Node::map();
    n.set("address", options.host);
    n.set("port", unsigned{options.port});
    n.set("mac", mac_text(radio.mac));
    n.set("board", radio.board);
    if (radio.version_minor) {
        n.set("gateware",
              std::to_string(radio.version_major) + "." + std::to_string(*radio.version_minor));
    } else {
        n.set("gateware", std::to_string(radio.version_major));
    }
    if (radio.protocol_version) {
        n.set("protocol_version", *radio.protocol_version);
    }
    n.set("receivers", radio.receivers);
    n.set("busy", radio.busy);
    n.set("discovery_ms", round_to(discovery_ms, 1));
    return n;
}

}  // namespace

std::string usage(Protocol protocol) {
    const bool one = protocol == Protocol::kOne;
    std::string text = std::string("usage: ") + name(protocol) +
                       " [options] [HOST]\n"
                       "\n"
                       "A minimal openHPSDR Protocol " +
                       (one ? "1 (Hermes-Lite 2 dialect)" : "2") +
                       " client for checking a radio or radioberryd:\n"
                       "it discovers the radio at HOST (default 127.0.0.1), streams from it, and\n"
                       "reports the packet rate, sequence breaks and receive levels. With --mox\n"
                       "it holds MOX through the measurement with drive 0 and zero samples. Exits\n"
                       "0 when every check passed, 1 when one failed or the radio did not answer,\n"
                       "2 on a usage error.\n"
                       "\n"
                       "  --port N            radio port" +
                       (one ? "" : " (base port)") +
                       ", default 1024\n"
                       "  --discover-only     discovery only, no stream\n"
                       "  --rate KSPS         48, 96, 192 or 384 (default 48)\n"
                       "  --receivers N       receivers to stream (default 1)\n"
                       "  --freq HZ           receive and transmit frequency (default 7074000)\n";
    if (one) {
        text += "  --lna DB            LNA gain, -12 to 48 (default: not sent)\n";
    }
    text +=
        "  --seconds S         measurement time (default 2)\n"
        "  --settle S          time streamed before measuring (default 0.2)\n"
        "  --tolerance PCT     largest packet rate error that passes (default 2)\n"
        "  --mox               zero-drive transmit during the measurement: this keys\n"
        "                      the transmitter if the radio allows it\n"
        "  --json              JSON output\n"
        "  --yaml              YAML output\n"
        "  --version           print the version\n"
        "  --help              this text\n";
    return text;
}

ParseResult parse_options(Protocol protocol, std::span<const std::string> args) {
    Options o;
    bool host_given = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            return ShowHelp{};
        }
        if (arg == "--version") {
            return ShowVersion{};
        }
        const auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size()) {
                return std::nullopt;
            }
            return args[++i];
        };
        if (arg == "--json") {
            o.format = ReportFormat::kJson;
        } else if (arg == "--yaml") {
            o.format = ReportFormat::kYaml;
        } else if (arg == "--discover-only") {
            o.discover_only = true;
        } else if (arg == "--mox") {
            o.mox = true;
        } else if (arg == "--port") {
            const auto v = value();
            const auto port = v ? number<unsigned>(*v) : std::nullopt;
            if (!port || *port == 0 || *port > 65535 - 20) {
                return UsageError{"--port needs a port number below 65515"};
            }
            o.port = static_cast<std::uint16_t>(*port);
        } else if (arg == "--rate") {
            const auto v = value();
            const auto rate = v ? number<unsigned>(*v) : std::nullopt;
            if (!rate || (*rate != 48 && *rate != 96 && *rate != 192 && *rate != 384)) {
                return UsageError{"--rate must be 48, 96, 192 or 384"};
            }
            o.stream.rate_ksps = *rate;
        } else if (arg == "--receivers") {
            const auto v = value();
            const auto n = v ? number<unsigned>(*v) : std::nullopt;
            if (!n || *n < 1 || *n > (protocol == Protocol::kOne ? 7u : 8u)) {
                return UsageError{protocol == Protocol::kOne ? "--receivers must be 1 to 7"
                                                             : "--receivers must be 1 to 8"};
            }
            o.stream.receivers = *n;
        } else if (arg == "--freq") {
            const auto v = value();
            const auto hz = v ? number<std::uint32_t>(*v) : std::nullopt;
            if (!hz || *hz > 61440000) {
                return UsageError{"--freq needs a frequency in Hz up to 61440000"};
            }
            o.stream.frequency_hz = *hz;
        } else if (arg == "--lna" && protocol == Protocol::kOne) {
            const auto v = value();
            const auto db = v ? number<int>(*v) : std::nullopt;
            if (!db || *db < -12 || *db > 48) {
                return UsageError{"--lna must be -12 to 48"};
            }
            o.stream.lna_db = db;
        } else if (arg == "--seconds" || arg == "--settle" || arg == "--tolerance") {
            const auto v = value();
            const auto x = v ? number<double>(*v) : std::nullopt;
            if (!x || !(*x >= 0) || *x > 3600 || (arg == "--seconds" && *x <= 0)) {
                return UsageError{arg + " needs a number from 0 to 3600" +
                                  (arg == "--seconds" ? ", above 0" : "")};
            }
            (arg == "--seconds"  ? o.seconds
             : arg == "--settle" ? o.settle
                                 : o.tolerance_percent) = *x;
        } else if (arg.starts_with("-")) {
            return UsageError{"unknown option " + arg};
        } else if (host_given) {
            return UsageError{"only one HOST"};
        } else {
            o.host = arg;
            host_given = true;
        }
    }
    return o;
}

Node probe(Protocol protocol, const Options& options, Client& client, const Clock& now) {
    Node report = Node::map();
    report.set("protocol", protocol == Protocol::kOne ? 1 : 2);
    Node findings = Node::seq();
    const auto finish = [&] {
        report.set("findings", findings);
        report.set("pass", findings.items().empty());
        return report;
    };

    // A radio that says it is busy may only be finishing with a client
    // that has just stopped: ask again for a little over a second.
    std::optional<Radio> radio;
    const nanoseconds asked = now();
    for (int attempt = 0; attempt < 5 && (!radio || radio->busy); ++attempt) {
        if (radio) {
            client.poll(milliseconds(250));
        }
        radio = client.discover(milliseconds(500));
    }
    if (!radio) {
        findings.push("no discovery reply from " + options.host);
        return finish();
    }
    report.set("radio", radio_node(options, *radio, seconds_since(now, asked) * 1000));
    if (radio->busy) {
        findings.push("the radio is streaming to another client");
        return finish();
    }
    if (options.discover_only) {
        return finish();
    }
    if (options.stream.receivers > radio->receivers) {
        findings.push("the radio has " + std::to_string(radio->receivers) + " receivers, not " +
                      std::to_string(options.stream.receivers));
        return finish();
    }

    client.start(options.stream);
    const auto run_for = [&](double seconds) {
        const nanoseconds from = now();
        while (seconds_since(now, from) < seconds) {
            client.poll(milliseconds(50));
        }
        return seconds_since(now, from);
    };
    run_for(options.settle);
    client.reset_counts();
    client.set_mox(options.mox);
    const double measured = run_for(options.seconds);
    const StreamCounts counts = client.counts();
    client.set_mox(false);
    run_for(std::min(options.settle, 0.2));
    client.stop();

    const double rate = static_cast<double>(counts.packets) / measured;
    const double expected = client.expected_packets_per_second();
    const double error_percent = (rate - expected) / expected * 100.0;
    Node stream = Node::map();
    stream.set("rate_ksps", options.stream.rate_ksps);
    stream.set("receivers", options.stream.receivers);
    stream.set("frequency_hz", options.stream.frequency_hz);
    stream.set("seconds", round_to(measured, 2));
    stream.set("packets", counts.packets);
    stream.set("packets_per_second", round_to(rate, 1));
    stream.set("expected_per_second", round_to(expected, 1));
    stream.set("rate_error_percent", round_to(error_percent, 2));
    stream.set("sequence_breaks", counts.sequence_breaks);
    stream.set("lost_packets", counts.lost_packets);
    Node levels = Node::seq();
    for (std::size_t n = 0; n < counts.levels.size(); ++n) {
        Node l = Node::map();
        l.set("receiver", n);
        l.set("rms_dbfs", level_db(counts.levels[n].rms_dbfs()));
        l.set("peak_dbfs", level_db(counts.levels[n].peak_dbfs()));
        levels.push(std::move(l));
    }
    stream.set("levels", levels);
    report.set("stream", stream);

    Node transmit = Node::map();
    transmit.set("mox", options.mox);
    transmit.set("drive", 0);
    transmit.set(protocol == Protocol::kOne ? "host_packets" : "tx_iq_packets", counts.tx_packets);
    transmit.set("ptt_reported_packets", counts.ptt_reported);
    report.set("transmit", transmit);

    if (counts.packets == 0) {
        findings.push("no receive stream packets");
    } else if (std::abs(error_percent) > options.tolerance_percent) {
        findings.push("packet rate " + std::to_string(rate) + " per second, expected " +
                      std::to_string(expected));
    }
    if (counts.sequence_breaks > 0) {
        findings.push(std::to_string(counts.sequence_breaks) + " sequence breaks, " +
                      std::to_string(counts.lost_packets) + " packets lost");
    }
    if (protocol == Protocol::kTwo && options.mox && counts.tx_packets == 0) {
        findings.push("no Mic/Line packets clocked the transmit stream");
    }
    return finish();
}

int run_app(Protocol protocol, std::span<const std::string> args, Environment& env) {
    const ParseResult parsed = parse_options(protocol, args);
    if (std::holds_alternative<ShowHelp>(parsed)) {
        env.out << usage(protocol);
        return kExitOk;
    }
    if (std::holds_alternative<ShowVersion>(parsed)) {
        env.out << name(protocol) << ' ' << kVersion << '\n';
        return kExitOk;
    }
    if (const auto* error = std::get_if<UsageError>(&parsed)) {
        env.err << name(protocol) << ": " << error->message << '\n' << usage(protocol);
        return kExitUsage;
    }
    const Options& options = std::get<Options>(parsed);

    Node report;
    try {
        const std::unique_ptr<Transport> transport = env.open(options.host, options.port);
        const std::unique_ptr<Client> client = protocol == Protocol::kOne
                                                   ? make_p1_client(*transport)
                                                   : make_p2_client(*transport, env.now);
        report = probe(protocol, options, *client, env.now);
    } catch (const std::exception& e) {
        env.err << name(protocol) << ": " << e.what() << '\n';
        return kExitFailed;
    }
    switch (options.format) {
        case ReportFormat::kText:
            env.out << render_text(report);
            break;
        case ReportFormat::kJson:
            env.out << render_json(report);
            break;
        case ReportFormat::kYaml:
            env.out << render_yaml(report);
            break;
    }
    return report.find("pass")->as_bool() ? kExitOk : kExitFailed;
}

}  // namespace radioberry::proto
