// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <charconv>
#include <cstdio>
#include <exception>

#include "radioberry/render.hpp"
#include "radioberry/version.hpp"

namespace radioberry::ctl {

const char* const kUsage =
    "usage: rbctl [options] [ADDRESS DATA]\n"
    "\n"
    "Sends one gateware command through /dev/radioberry and prints the\n"
    "command, the gateware's reply, and the driver's info and statistics.\n"
    "Without ADDRESS and DATA it prints the info and statistics only.\n"
    "ADDRESS is a Hermes-Lite 2 command address (0 to 0x3f) and DATA the\n"
    "32-bit C1..C4 value, each decimal or 0x-prefixed hex. Exits 0 on\n"
    "success, 1 on a device error, 2 on a usage error or a refused command.\n"
    "\n"
    "  --ptt          set PTT (needs --allow-tx)\n"
    "  --tx-permit    set the TX permission bit (needs --allow-tx)\n"
    "  --cwx          set CWX (needs --allow-tx)\n"
    "  --no-run       clear the run bit, which is set by default\n"
    "  --allow-tx     allow a command that can key the transmitter or arm\n"
    "                 the PA: PTT, TX permission, CWX, or at address 0x09\n"
    "                 drive, VNA or PA enable\n"
    "  --device PATH  default /dev/radioberry\n"
    "  --json         JSON output\n"
    "  --yaml         YAML output\n"
    "  --version      print the version\n"
    "  --help         this text\n"
    "\n"
    "The device is exclusive, so rbctl fails while rbcat or the daemon has\n"
    "it open. When rbctl exits, closing the device sends the gateware a\n"
    "command that clears run, PTT, the TX permission bit, PA enable and\n"
    "drive; other settings such as frequencies stay. A PTT hang time of 31\n"
    "at address 0x17 disables the gateware's transmit watchdog and is\n"
    "always refused.\n";

namespace {

std::optional<std::uint32_t> parse_u32(const std::string& text) {
    std::string_view digits = text;
    int base = 10;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
        base = 16;
    }
    std::uint32_t value = 0;
    const char* end = digits.data() + digits.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): bounded by `end`
    const auto [ptr, ec] = std::from_chars(digits.data(), end, value, base);
    if (digits.empty() || ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::string hex(std::uint32_t value, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%0*x", digits, value);
    return buf;
}

Node command_node(const Command& c) {
    Node n = Node::map();
    n.set("address", hex(c.address, 2));
    n.set("data", hex(c.data, 8));
    n.set("run", (c.flags & Command::kRun) != 0);
    n.set("ptt", c.ptt);
    n.set("tx_permission", (c.flags & Command::kTxPermit) != 0);
    n.set("cwx", (c.flags & Command::kCwx) != 0);
    std::string bytes;
    for (const std::uint8_t b : c.encode()) {
        char buf[4];
        std::snprintf(buf, sizeof buf, "%02x", b);
        bytes += (bytes.empty() ? "" : " ") + std::string(buf);
    }
    n.set("bytes", bytes);
    Node hazards = Node::seq();
    for (const std::string& h : transmit_hazards(c)) {
        hazards.push(h);
    }
    n.set("transmit_hazards", hazards);
    return n;
}

Node reply_node(const Reply& r) {
    Node n = Node::map();
    n.set("external_cw_key", r.external_cw_key);
    n.set("cw_or_ptt", r.cw_or_ptt);
    n.set("gateware", version_text(r.version_major, r.version_minor));
    n.set("fpga", fpga_text(r.fpga_type));
    n.set("receivers", r.receivers);
    n.set("transmitters", r.transmitters);
    return n;
}

Node info_node(const radioberry_info& i) {
    Node n = Node::map();
    n.set("abi_version", i.abi_version);
    n.set("driver_version", driver_version_text(i.driver_version));
    n.set("gateware", version_text(i.gateware_major, i.gateware_minor));
    n.set("fpga", fpga_text(i.fpga_type));
    n.set("receivers", unsigned{i.receivers});
    n.set("transmitters", unsigned{i.transmitters});
    n.set("active_receivers", unsigned{i.active_receivers});
    if (i.rx_rate_code < kRxRatesKsps.size()) {
        n.set("rx_rate_ksps", kRxRatesKsps[i.rx_rate_code]);
    } else {
        n.set("rx_rate_ksps", Node());
    }
    // Drivers up to release 0.3.0 report neither (0).
    switch (i.transport) {
        case 0:
            n.set("transport", Node());
            break;
        case RADIOBERRY_TRANSPORT_PIO:
            n.set("transport", "rp1-pio");
            break;
        case RADIOBERRY_TRANSPORT_CPU:
            n.set("transport", "cpu");
            break;
        case RADIOBERRY_TRANSPORT_SMI:
            n.set("transport", "smi");
            break;
        default:
            n.set("transport", "unknown (" + std::to_string(i.transport) + ")");
            break;
    }
    if (i.rx_values_max != 0) {
        n.set("rx_values_max", i.rx_values_max);
    } else {
        n.set("rx_values_max", Node());
    }
    return n;
}

Node stats_node(const radioberry_stats& s) {
    Node n = Node::map();
    n.set("rx_words", s.rx_words);
    n.set("rx_frames", s.rx_frames);
    n.set("rx_resyncs", s.rx_resyncs);
    n.set("rx_dropped_words", s.rx_dropped_words);
    n.set("rx_overrun_frames", s.rx_overrun_frames);
    n.set("rx_discarded_words", s.rx_discarded_words);
    n.set("tx_words", s.tx_words);
    n.set("tx_underruns", s.tx_underruns);
    n.set("commands", s.commands);
    n.set("rx_ring_frames", s.rx_ring_frames);
    n.set("tx_ring_words", s.tx_ring_words);
    n.set("rx_period_bytes", s.rx_period_bytes);
    n.set("rx_overrun_flag", (s.flags & RADIOBERRY_STATS_RX_OVERRUN) != 0);
    n.set("ptt_flag", (s.flags & RADIOBERRY_STATS_PTT) != 0);
    return n;
}

}  // namespace

ParseResult parse_options(std::span<const std::string> args) {
    Options o;
    Command c;
    std::vector<std::string> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
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
        } else if (arg == "--ptt") {
            c.ptt = true;
        } else if (arg == "--tx-permit") {
            c.flags |= Command::kTxPermit;
        } else if (arg == "--cwx") {
            c.flags |= Command::kCwx;
        } else if (arg == "--no-run") {
            c.flags &= static_cast<std::uint8_t>(~Command::kRun);
        } else if (arg == "--allow-tx") {
            o.allow_tx = true;
        } else if (arg == "--device") {
            if (i + 1 >= args.size()) {
                return UsageError{"--device needs a path"};
            }
            o.device = args[++i];
        } else if (arg.starts_with("-") && arg != "-") {
            return UsageError{"unknown option " + arg};
        } else {
            positional.push_back(arg);
        }
    }

    const bool flags_given = c.ptt || c.flags != Command::kRun;
    if (positional.empty()) {
        if (flags_given) {
            return UsageError{"--ptt, --tx-permit, --cwx and --no-run need ADDRESS and DATA"};
        }
        return o;
    }
    if (positional.size() != 2) {
        return UsageError{"expected ADDRESS and DATA"};
    }
    const auto address = parse_u32(positional[0]);
    if (!address || *address > 0x3f) {
        return UsageError{"ADDRESS must be 0 to 0x3f, not " + positional[0]};
    }
    const auto data = parse_u32(positional[1]);
    if (!data) {
        return UsageError{"DATA must be a 32-bit number, not " + positional[1]};
    }
    c.address = static_cast<std::uint8_t>(*address);
    c.data = *data;
    o.command = c;
    return o;
}

std::optional<std::string> refusal(const Options& options) {
    if (!options.command) {
        return std::nullopt;
    }
    if (disables_tx_watchdog(*options.command)) {
        return "refusing PTT hang time 31: it disables the gateware's transmit watchdog";
    }
    const std::vector<std::string> hazards = transmit_hazards(*options.command);
    if (!hazards.empty() && !options.allow_tx) {
        std::string list;
        for (const std::string& h : hazards) {
            list += (list.empty() ? "" : ", ") + h;
        }
        return "refusing a command that can transmit (" + list + ") without --allow-tx";
    }
    return std::nullopt;
}

Node build_report(const Options& options, const std::optional<Reply>& reply,
                  const radioberry_info& info, const radioberry_stats& stats) {
    Node root = Node::map();
    root.set("device", options.device);
    if (options.command) {
        root.set("command", command_node(*options.command));
    }
    if (reply) {
        root.set("reply", reply_node(*reply));
    }
    root.set("info", info_node(info));
    root.set("statistics", stats_node(stats));
    return root;
}

int run_app(std::span<const std::string> args, Environment& env) {
    const ParseResult parsed = parse_options(args);
    if (std::holds_alternative<ShowHelp>(parsed)) {
        env.out << kUsage;
        return kExitOk;
    }
    if (std::holds_alternative<ShowVersion>(parsed)) {
        env.out << "rbctl " << kVersion << '\n';
        return kExitOk;
    }
    if (const auto* error = std::get_if<UsageError>(&parsed)) {
        env.err << "rbctl: " << error->message << '\n' << kUsage;
        return kExitUsage;
    }
    const Options& options = std::get<Options>(parsed);
    if (const auto why = refusal(options)) {
        env.err << "rbctl: " << *why << '\n';
        return kExitUsage;
    }

    Node report;
    try {
        const std::unique_ptr<Device> device = env.open(options.device);
        std::optional<Reply> reply;
        if (options.command) {
            reply = device->command(*options.command);
        }
        const radioberry_info info = device->info();
        const radioberry_stats stats = device->stats();
        report = build_report(options, reply, info, stats);
    } catch (const std::exception& e) {
        env.err << "rbctl: " << e.what() << '\n';
        return kExitDeviceError;
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
    return kExitOk;
}

}  // namespace radioberry::ctl
