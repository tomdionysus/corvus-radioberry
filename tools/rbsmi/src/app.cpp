// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <sstream>
#include <vector>

#include "radioberry/board.hpp"
#include "radioberry/node.hpp"
#include "radioberry/render.hpp"
#include "radioberry/version.hpp"
#include "rb_smi.h"

namespace radioberry::smi {

const char* const kUsage =
    "usage: rbsmi [options]\n"
    "\n"
    "Measures the RadioBerry's RX bus read by the BCM2711's SMI on a Raspberry\n"
    "Pi 4: each SMI read strobe on GPIO 6 is one RX clock, the SMI FIFO is\n"
    "polled through /dev/mem, and the words are parsed as the driver would\n"
    "(driver/kernel/rb_smi.h). Receive only: PTT, the TX permission bit and\n"
    "CWX stay clear, and the run ends with a command that clears run, PTT, the\n"
    "TX permission bit, PA enable and drive. Needs the gateware loaded\n"
    "(rbload), the radioberry overlay off, dtparam=spi=on and the\n"
    "rbsmi-measure overlay (docs/tools.md). Exits 0 when every frame arrived\n"
    "in sequence, 1 when frames were lost or the run could not start, 2 on a\n"
    "usage error.\n"
    "\n"
    "  --seconds S         how long to stream (default 10)\n"
    "  --receivers N       receivers, 1 to 6 (default 1)\n"
    "  --rate KSPS         48, 96, 192 or 384 (default 48)\n"
    "  --timing S,T,H,P    read setup, strobe, hold and pace in SMI clock\n"
    "                      cycles (default 2,10,2,16: 30 cycles, 4.17 million\n"
    "                      strobes per second at 125 MHz)\n"
    "  --read-words N      words per SMI transfer (default 16384)\n"
    "  --spi DEVICE        default /dev/spidev0.0\n"
    "  --json              JSON output\n"
    "  --yaml              YAML output\n"
    "  --version           print the version\n"
    "  --help              this text\n";

namespace {

constexpr unsigned kMaxReceivers = 6;
constexpr double kSmiClockHz = 125e6;
// A freshly loaded gateware answers with a zero identity for a few seconds
// (as rbbus): poll for this long before giving up.
constexpr std::chrono::seconds kIdentifyWait{10};
constexpr std::chrono::milliseconds kIdentifyPoll{100};
// Frames that may be missing at the start (settling) and end.
constexpr double kFrameTolerance = 0.02;

std::optional<unsigned> parse_unsigned(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos ||
        text.size() > 9) {
        return std::nullopt;
    }
    return static_cast<unsigned>(std::stoul(text));
}

std::optional<double> parse_seconds(const std::string& text) {
    try {
        std::size_t used = 0;
        const double v = std::stod(text, &used);
        if (used != text.size() || !(v > 0) || v > 86400) {
            return std::nullopt;
        }
        return v;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Timing> parse_timing(const std::string& text) {
    std::vector<unsigned> v;
    std::istringstream in(text);
    std::string part;
    while (std::getline(in, part, ',')) {
        const auto n = parse_unsigned(part);
        if (!n) {
            return std::nullopt;
        }
        v.push_back(*n);
    }
    if (v.size() != 4 || v[0] < 1 || v[0] > 63 || v[1] < 1 || v[1] > 127 || v[2] > 63 ||
        v[3] > 127) {
        return std::nullopt;
    }
    return Timing{v[0], v[1], v[2], v[3]};
}

std::string hex(std::uint32_t v) {
    std::ostringstream s;
    s << "0x" << std::hex << v;
    return s.str();
}

void render(const Options& options, const Node& report, std::ostream& out) {
    switch (options.format) {
        case ReportFormat::kText:
            out << render_text(report);
            break;
        case ReportFormat::kJson:
            out << render_json(report);
            break;
        case ReportFormat::kYaml:
            out << render_yaml(report);
            break;
    }
}

double round2(double v) { return std::round(v * 100) / 100; }

Command safe_command() {
    Command c;
    c.flags = 0;
    c.address = RB_ADDR_PA;
    c.data = 0;
    return c;
}

class Run {
public:
    Run(const Options& options, Environment& env, Reader& reader, Control& control)
        : opt_(options), env_(env), reader_(reader), control_(control) {}

    // Sends a command; never one that could transmit.
    Reply send(const Command& c) {
        if (c.can_transmit() || !transmit_hazards(c).empty()) {
            throw std::logic_error("rbsmi never sends a command that can transmit");
        }
        return Reply::decode(control_.transfer(c.encode()));
    }

    int run(Node& report) {
        const unsigned rate_hz = kRxRatesKsps[opt_.rate_code] * 1000;

        Reply id = send(safe_command());
        const auto give_up = env_.now() + kIdentifyWait;
        while ((id.version_major == 0 || id.receivers == 0) && env_.now() < give_up) {
            env_.sleep(kIdentifyPoll);
            id = send(safe_command());
        }
        Node gw = Node::map();
        gw.set("version", version_text(id.version_major, id.version_minor));
        gw.set("fpga", fpga_text(id.fpga_type));
        gw.set("receivers", id.receivers);
        report.set("gateware", gw);
        if (id.version_major == 0 || id.receivers == 0) {
            env_.err << "rbsmi: no gateware answers on " << opt_.spi
                     << "; load it with rbload first\n";
            return kExitFailed;
        }
        if (opt_.receivers > id.receivers) {
            env_.err << "rbsmi: the gateware has " << id.receivers << " receivers\n";
            return kExitFailed;
        }

        reader_.configure(opt_.timing);
        send(hl2::general(opt_.rate_code, opt_.receivers));

        rb_smi_parser smi;
        rb_smi_init(&smi);
        rb_rx_parser parser;
        rb_rx_init(&parser, opt_.receivers);
        rb_rx_set_receivers(&parser, opt_.receivers);  // settle past older values
        std::vector<std::uint32_t> words(opt_.read_words);
        std::vector<std::uint32_t> values(rb_smi_max_values(opt_.read_words));
        std::uint64_t frames = 0, reads = 0, short_reads = 0;
        std::uint32_t bits_any = 0, bits_all = ~0u;

        const double cpu_start = env_.cpu_seconds();
        const auto start = env_.now();
        double elapsed = 0;
        try {
            while (elapsed < opt_.seconds) {
                const std::size_t n = reader_.read(words);
                ++reads;
                if (n < words.size()) {
                    ++short_reads;
                }
                for (std::size_t i = 0; i < n; ++i) {
                    bits_any |= words[i];
                    bits_all &= words[i];
                }
                const unsigned got =
                    rb_smi_parse(&smi, words.data(), static_cast<unsigned>(n), values.data());
                for (unsigned i = 0; i < got; ++i) {
                    if (rb_rx_feed(&parser, values[i])) {
                        ++frames;
                    }
                }
                elapsed = std::chrono::duration<double>(env_.now() - start).count();
            }
        } catch (...) {
            send(safe_command());
            throw;
        }
        const double cpu = env_.cpu_seconds() - cpu_start;
        send(safe_command());

        const double nominal = kSmiClockHz / opt_.timing.cycles();
        Node strobes = Node::map();
        strobes.set("timing", std::to_string(opt_.timing.setup) + "," +
                                  std::to_string(opt_.timing.strobe) + "," +
                                  std::to_string(opt_.timing.hold) + "," +
                                  std::to_string(opt_.timing.pace));
        strobes.set("nominal_per_second", std::round(nominal));
        strobes.set("per_second", std::round(static_cast<double>(smi.strobes) / elapsed));
        strobes.set("duty_percent",
                    round2(100 * static_cast<double>(smi.strobes) / elapsed / nominal));
        strobes.set("reads", reads);
        strobes.set("short_reads", short_reads);
        strobes.set("bits_seen", hex(bits_any));
        strobes.set("bits_always", hex(bits_all));
        report.set("smi", strobes);

        const double expected = elapsed * rate_hz;
        Node rx = Node::map();
        rx.set("receivers", opt_.receivers);
        rx.set("rate_ksps", kRxRatesKsps[opt_.rate_code]);
        rx.set("values", smi.values);
        rx.set("values_per_second", std::round(static_cast<double>(smi.values) / elapsed));
        rx.set("idle_strobes", smi.idle);
        rx.set("frames", frames);
        rx.set("frames_expected", std::round(expected));
        rx.set("resyncs", parser.resyncs);
        rx.set("dropped_words", parser.dropped);
        rx.set("discarded_words", parser.discarded);
        report.set("rx", rx);
        Node run = Node::map();
        run.set("seconds", round2(elapsed));
        run.set("cpu_seconds", round2(cpu));
        run.set("cpu_percent", round2(100 * cpu / elapsed));
        report.set("run", run);

        const bool complete = static_cast<double>(frames) >= expected * (1 - kFrameTolerance);
        const bool in_sequence = parser.resyncs == 0 && parser.dropped == 0;
        report.set("pass", complete && in_sequence);
        return complete && in_sequence ? kExitOk : kExitFailed;
    }

private:
    const Options& opt_;
    Environment& env_;
    Reader& reader_;
    Control& control_;
};

}  // namespace

ParseResult parse_options(std::span<const std::string> args) {
    Options o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto value = [&]() -> const std::string* {
            return i + 1 < args.size() ? &args[++i] : nullptr;
        };
        if (arg == "--help" || arg == "-h") {
            return ShowHelp{};
        }
        if (arg == "--version") {
            return ShowVersion{};
        }
        if (arg == "--json") {
            o.format = ReportFormat::kJson;
        } else if (arg == "--yaml") {
            o.format = ReportFormat::kYaml;
        } else if (arg == "--seconds") {
            const std::string* v = value();
            const auto s = v ? parse_seconds(*v) : std::nullopt;
            if (!s) {
                return UsageError{"--seconds needs a number of seconds above 0"};
            }
            o.seconds = *s;
        } else if (arg == "--receivers") {
            const std::string* v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n || *n < 1 || *n > kMaxReceivers) {
                return UsageError{"--receivers needs 1 to 6"};
            }
            o.receivers = *n;
        } else if (arg == "--rate") {
            const std::string* v = value();
            const auto ksps = v ? parse_unsigned(*v) : std::nullopt;
            const auto code = ksps ? rate_code_for_ksps(*ksps) : std::nullopt;
            if (!code) {
                return UsageError{"--rate needs 48, 96, 192 or 384"};
            }
            o.rate_code = *code;
        } else if (arg == "--timing") {
            const std::string* v = value();
            const auto t = v ? parse_timing(*v) : std::nullopt;
            if (!t) {
                return UsageError{
                    "--timing needs setup,strobe,hold,pace: 1-63, 1-127, 0-63, 0-127"};
            }
            o.timing = *t;
        } else if (arg == "--read-words") {
            const std::string* v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n || *n < 64 || *n > (1u << 20)) {
                return UsageError{"--read-words needs 64 to 1048576"};
            }
            o.read_words = *n;
        } else if (arg == "--spi") {
            const std::string* v = value();
            if (!v) {
                return UsageError{"--spi needs a device"};
            }
            o.spi = *v;
        } else {
            return UsageError{"unknown argument " + arg};
        }
    }
    return o;
}

int run_app(std::span<const std::string> args, Environment& env) {
    const ParseResult parsed = parse_options(args);
    if (std::holds_alternative<ShowHelp>(parsed)) {
        env.out << kUsage;
        return kExitOk;
    }
    if (std::holds_alternative<ShowVersion>(parsed)) {
        env.out << "rbsmi " << kVersion << '\n';
        return kExitOk;
    }
    if (const auto* error = std::get_if<UsageError>(&parsed)) {
        env.err << "rbsmi: " << error->message << '\n' << kUsage;
        return kExitUsage;
    }
    const Options& options = std::get<Options>(parsed);

    const Board board = board_from_compatible(env.compatible());
    if (board != Board::kPi4) {
        env.err << "rbsmi: this is not a Raspberry Pi 4 (BCM2711); SMI on these pins is a Pi 4 "
                   "measurement\n";
        return kExitUsage;
    }
    if (env.driver_bound()) {
        env.err << "rbsmi: /dev/radioberry exists, so the driver owns the bus pins; remove "
                   "dtoverlay=radioberry, set dtparam=spi=on and reboot\n";
        return kExitUsage;
    }

    Node report = Node::map();
    report.set("board", std::string(to_string(board)));
    int rc = kExitFailed;
    try {
        const std::unique_ptr<Control> control = env.open_control(options.spi);
        const std::unique_ptr<Reader> reader = env.open_reader();
        Run run(options, env, *reader, *control);
        rc = run.run(report);
    } catch (const std::exception& e) {
        env.err << "rbsmi: " << e.what() << '\n';
        return kExitFailed;
    }
    render(options, report, env.out);
    return rc;
}

}  // namespace radioberry::smi
