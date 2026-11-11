// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <vector>

#include "radioberry/board.hpp"
#include "radioberry/node.hpp"
#include "radioberry/render.hpp"
#include "radioberry/version.hpp"
#include "rb_logic.h"

namespace radioberry::bus {

const char* const kUsage =
    "usage: rbbus [options]\n"
    "\n"
    "Measures the RadioBerry's RX bus, and optionally its TX bus, clocked by\n"
    "the CPU through /dev/gpiomem on a Raspberry Pi 4: the bus code the\n"
    "driver uses on a Pi 4 (driver/kernel/rb_bitbang.h), run from userspace.\n"
    "Receive only: PTT, the TX permission bit and CWX stay clear, and the\n"
    "run ends with a command that clears run, PTT, the TX permission bit, PA\n"
    "enable and drive. Needs the gateware loaded (rbload), the radioberry\n"
    "overlay off and spidev on (dtparam=spi=on), so that /dev/radioberry is\n"
    "absent. Exits 0 when every frame arrived in sequence, 1 when frames were\n"
    "lost or the run could not start, 2 on a usage error.\n"
    "\n"
    "  --seconds S         how long to stream (default 10)\n"
    "  --receivers N       receivers, 1 to 6 (default 1)\n"
    "  --rate KSPS         48, 96, 192 or 384 (default 48)\n"
    "  --settle N          extra pin reads after each clock pulse (default 0)\n"
    "  --idle-sleep US     sleep when the gateware has nothing to send\n"
    "                      (default 1000; 0 spins)\n"
    "  --tx                also clock TX words of zeros at 48 000 per second\n"
    "  --spi DEVICE        default /dev/spidev0.0\n"
    "  --json              JSON output\n"
    "  --yaml              YAML output\n"
    "  --version           print the version\n"
    "  --help              this text\n"
    "\n"
    "Run it with a real-time priority (chrt -f 50 rbbus ...) to measure what\n"
    "the driver's thread would see.\n";

namespace {

constexpr unsigned kRxBatchWords = 1024;
constexpr unsigned kRxMaxIdle = 64;
constexpr unsigned kTxBatchWords = 256;
constexpr unsigned kTxMaxPolls = 64;
constexpr unsigned kTxWordsPerSecond = 48000;
constexpr unsigned kMaxReceivers = 6;
// The gateware holds its RX state still around a receiver-count change
// only if the RX clock is still (docs/fault-rx-lockup.md).
constexpr std::chrono::microseconds kClockHold{200};
// A freshly loaded gateware answers with a zero identity for a few seconds
// (measured on a Pi 4 after rbload; gateware/README.md advises polling):
// poll for this long before giving up.
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

class Run {
public:
    Run(const Options& options, Environment& env, Bus& bus, Control& control)
        : opt_(options), env_(env), bus_(bus), control_(control) {}

    // Sends a command; never one that could transmit.
    Reply send(const Command& c) {
        if (c.can_transmit() || !transmit_hazards(c).empty()) {
            throw std::logic_error("rbbus never sends a command that can transmit");
        }
        const auto reply = control_.transfer(c.encode());
        return Reply::decode(reply);
    }

    int run(Node& report) {
        const unsigned rate_hz = kRxRatesKsps[opt_.rate_code] * 1000;
        bus_.set_settle_reads(opt_.settle_reads);

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
            env_.err << "rbbus: no gateware answers on " << opt_.spi
                     << "; load it with rbload first\n";
            return kExitFailed;
        }
        if (opt_.receivers > id.receivers) {
            env_.err << "rbbus: the gateware has " << id.receivers << " receivers\n";
            return kExitFailed;
        }

        // The bus is still here: nothing clocks it between runs.
        send(hl2::general(opt_.rate_code, opt_.receivers));
        env_.sleep(kClockHold);

        rb_rx_parser parser;
        rb_rx_init(&parser, opt_.receivers);
        rb_rx_set_receivers(&parser, opt_.receivers);  // settle past older values
        std::vector<std::uint32_t> words(kRxBatchWords);
        const std::vector<std::uint32_t> zeros(kTxBatchWords, 0);
        std::uint64_t frames = 0, tx_sent = 0;

        const double cpu_start = env_.cpu_seconds();
        const auto start = env_.now();
        double elapsed = 0;
        try {
            while (elapsed < opt_.seconds) {
                const unsigned got = bus_.rx_run(words.data(), kRxBatchWords, kRxMaxIdle);
                for (unsigned i = 0; i < got; ++i) {
                    if (rb_rx_feed(&parser, words[i])) {
                        ++frames;
                    }
                }
                bool tx_due = false;
                if (opt_.tx) {
                    const auto due = static_cast<std::uint64_t>(elapsed * kTxWordsPerSecond);
                    if (due > tx_sent) {
                        const auto n = static_cast<unsigned>(
                            std::min<std::uint64_t>(due - tx_sent, kTxBatchWords));
                        tx_sent += bus_.tx_run(zeros.data(), n, kTxMaxPolls);
                        tx_due = true;
                    }
                }
                if (got == 0 && !tx_due && opt_.idle_sleep_us > 0) {
                    env_.sleep(std::chrono::microseconds(opt_.idle_sleep_us));
                }
                elapsed = std::chrono::duration<double>(env_.now() - start).count();
            }
        } catch (...) {
            send(safe_command());
            throw;
        }
        const double cpu = env_.cpu_seconds() - cpu_start;
        send(safe_command());

        const BusCounters c = bus_.counters();
        const double expected = elapsed * rate_hz;
        Node rx = Node::map();
        rx.set("receivers", opt_.receivers);
        rx.set("rate_ksps", kRxRatesKsps[opt_.rate_code]);
        rx.set("words", c.rx_words);
        rx.set("values_per_second", std::round(static_cast<double>(c.rx_words) / elapsed));
        rx.set("frames", frames);
        rx.set("frames_expected", std::round(expected));
        rx.set("resyncs", parser.resyncs);
        rx.set("dropped_words", parser.dropped);
        rx.set("discarded_words", parser.discarded);
        rx.set("clocks", c.rx_clocks);
        rx.set("idle_clocks", c.rx_idle_clocks);
        rx.set("clocks_per_second", std::round(static_cast<double>(c.rx_clocks) / elapsed));
        report.set("rx", rx);
        if (opt_.tx) {
            Node tx = Node::map();
            tx.set("words", c.tx_words);
            tx.set("words_per_second", std::round(static_cast<double>(c.tx_words) / elapsed));
            tx.set("ready_waits", c.tx_ready_waits);
            report.set("tx", tx);
        }
        Node run = Node::map();
        run.set("seconds", round2(elapsed));
        run.set("cpu_seconds", round2(cpu));
        run.set("cpu_percent", round2(100 * cpu / elapsed));
        run.set("settle_reads", opt_.settle_reads);
        run.set("idle_sleep_us", opt_.idle_sleep_us);
        report.set("run", run);

        const bool complete = static_cast<double>(frames) >= expected * (1 - kFrameTolerance);
        const bool in_sequence = parser.resyncs == 0 && parser.dropped == 0;
        const bool tx_kept_up = !opt_.tx || static_cast<double>(c.tx_words) >=
                                                elapsed * kTxWordsPerSecond * (1 - kFrameTolerance);
        report.set("pass", complete && in_sequence && tx_kept_up);
        return complete && in_sequence && tx_kept_up ? kExitOk : kExitFailed;
    }

private:
    const Options& opt_;
    Environment& env_;
    Bus& bus_;
    Control& control_;
};

}  // namespace

Command safe_command() {
    Command c;
    c.flags = 0;
    c.address = RB_ADDR_PA;
    c.data = 0;
    return c;
}

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
        } else if (arg == "--tx") {
            o.tx = true;
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
        } else if (arg == "--settle") {
            const std::string* v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n || *n > 1000) {
                return UsageError{"--settle needs 0 to 1000"};
            }
            o.settle_reads = *n;
        } else if (arg == "--idle-sleep") {
            const std::string* v = value();
            const auto n = v ? parse_unsigned(*v) : std::nullopt;
            if (!n || *n > 1000000) {
                return UsageError{"--idle-sleep needs 0 to 1000000 microseconds"};
            }
            o.idle_sleep_us = *n;
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
        env.out << "rbbus " << kVersion << '\n';
        return kExitOk;
    }
    if (const auto* error = std::get_if<UsageError>(&parsed)) {
        env.err << "rbbus: " << error->message << '\n' << kUsage;
        return kExitUsage;
    }
    const Options& options = std::get<Options>(parsed);

    const Board board = board_from_compatible(env.compatible());
    if (board != Board::kPi4) {
        env.err << "rbbus: this is not a Raspberry Pi 4 (BCM2711); its GPIO registers are the "
                   "only ones rbbus drives\n";
        return kExitUsage;
    }
    if (env.driver_bound()) {
        env.err << "rbbus: /dev/radioberry exists, so the driver owns the bus pins; remove "
                   "dtoverlay=radioberry, set dtparam=spi=on and reboot\n";
        return kExitUsage;
    }

    Node report = Node::map();
    report.set("board", std::string(to_string(board)));
    int rc = kExitFailed;
    try {
        const std::unique_ptr<Control> control = env.open_control(options.spi);
        const std::unique_ptr<Bus> bus = env.open_bus();
        Run run(options, env, *bus, *control);
        rc = run.run(report);
    } catch (const std::exception& e) {
        env.err << "rbbus: " << e.what() << '\n';
        return kExitFailed;
    }
    render(options, report, env.out);
    return rc;
}

}  // namespace radioberry::bus
