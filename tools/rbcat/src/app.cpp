// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <cmath>
#include <exception>

#include "radioberry/render.hpp"
#include "radioberry/version.hpp"

namespace radioberry::cat {
namespace {

// Rounded to `scale` steps per unit; dividing keeps 0.1 steps exact in text.
double round_to(double value, double scale) { return std::round(value * scale) / scale; }

Node rounded(const std::optional<double>& value) {
    return value ? Node(round_to(*value, 10)) : Node();
}

Node rx_counters(const radioberry_stats& from, const radioberry_stats& to) {
    Node n = Node::map();
    n.set("rx_resyncs", to.rx_resyncs - from.rx_resyncs);
    n.set("rx_dropped_words", to.rx_dropped_words - from.rx_dropped_words);
    n.set("rx_overrun_frames", to.rx_overrun_frames - from.rx_overrun_frames);
    n.set("rx_discarded_words", to.rx_discarded_words - from.rx_discarded_words);
    return n;
}

}  // namespace

bool tx_clean(const Options& options, const StreamResult& result) {
    if (!options.tx_zeros) {
        return true;
    }
    const radioberry_stats& a = result.measure_start;
    const radioberry_stats& b = result.end;
    if (b.tx_underruns != a.tx_underruns || result.tx_ring_min_words.value_or(0) == 0) {
        return false;
    }
    if (result.seconds < 1) {
        return true;
    }
    const double rate = static_cast<double>(b.tx_words - a.tx_words) / result.seconds;
    return std::abs(rate - kTxRate) <= kTxRate * 0.01;
}

bool stream_clean(const Options& options, const StreamResult& result) {
    const radioberry_stats& a = result.measure_start;
    const radioberry_stats& b = result.end;
    if (b.rx_resyncs != a.rx_resyncs || b.rx_dropped_words != a.rx_dropped_words ||
        b.rx_overrun_frames != a.rx_overrun_frames || result.frames == 0) {
        return false;
    }
    if (result.seconds < 1) {
        return tx_clean(options, result);
    }
    const double expected = options.rate_ksps * 1000.0;
    const double rate = static_cast<double>(result.frames) / result.seconds;
    return std::abs(rate - expected) <= expected * 0.01 && tx_clean(options, result);
}

Node build_report(const Options& options, const StreamResult& result) {
    Node root = Node::map();

    Node device = Node::map();
    device.set("path", options.device);
    device.set("driver_version", driver_version_text(result.info.driver_version));
    device.set("abi_version", result.info.abi_version);
    device.set("gateware", version_text(result.info.gateware_major, result.info.gateware_minor));
    device.set("fpga", fpga_text(result.info.fpga_type));
    device.set("gateware_receivers", unsigned{result.info.receivers});
    root.set("device", device);

    Node settings = Node::map();
    settings.set("rate_ksps", options.rate_ksps);
    settings.set("receivers", options.receivers);
    Node freqs = Node::seq();
    for (const std::uint32_t hz : options.frequencies_hz) {
        freqs.push(unsigned{hz});
    }
    settings.set("frequencies_hz", freqs);
    settings.set("lna_gain_db", options.lna_gain_db);
    settings.set("output", to_string(options.output));
    settings.set("settle_seconds", options.settle_seconds);
    settings.set("seconds", options.seconds > 0 ? Node(options.seconds) : Node());
    settings.set("tx_zeros", options.tx_zeros);
    root.set("settings", settings);

    Node stream = Node::map();
    stream.set("seconds", round_to(result.seconds, 100));
    stream.set("frames", result.frames);
    stream.set("frame_rate",
               result.seconds > 0
                   ? Node(round_to(static_cast<double>(result.frames) / result.seconds, 10))
                   : Node());
    stream.set("expected_frame_rate", options.rate_ksps * 1000u);
    stream.set("bytes_written", result.bytes_out);
    stream.set("interrupted", result.interrupted);
    root.set("stream", stream);

    Node levels = Node::seq();
    for (std::size_t r = 0; r < result.levels.size(); ++r) {
        Node l = Node::map();
        l.set("receiver", static_cast<unsigned>(r + 1));
        l.set("rms_dbfs", rounded(result.levels[r].rms_dbfs()));
        l.set("peak_dbfs", rounded(result.levels[r].peak_dbfs()));
        levels.push(l);
    }
    root.set("levels", levels);

    // Driver counters over the measurement, then over the settling time.
    Node driver = rx_counters(result.measure_start, result.end);
    driver.set("rx_words", result.end.rx_words - result.measure_start.rx_words);
    driver.set("rx_frames", result.end.rx_frames - result.measure_start.rx_frames);
    driver.set("rx_period_bytes", unsigned{result.end.rx_period_bytes});
    root.set("driver", driver);
    root.set("settling", rx_counters(result.before, result.measure_start));

    if (options.tx_zeros) {
        const std::uint64_t words = result.end.tx_words - result.measure_start.tx_words;
        Node tx = Node::map();
        tx.set("tx_words", words);
        tx.set("tx_word_rate", result.seconds > 0
                                   ? Node(round_to(static_cast<double>(words) / result.seconds, 10))
                                   : Node());
        tx.set("expected_tx_word_rate", static_cast<unsigned>(kTxRate));
        tx.set("tx_underruns", result.end.tx_underruns - result.measure_start.tx_underruns);
        tx.set("tx_ring_min_words", result.tx_ring_min_words);
        tx.set("tx_ring_target_words", kTxTargetWords);
        tx.set("bytes_written_to_device", result.tx_bytes);
        root.set("tx", tx);
    }

    Node safety = Node::map();
    bool ptt = false;
    bool permit = false;
    for (const Command& c : result.commands) {
        ptt = ptt || c.ptt;
        permit = permit || (c.flags & Command::kTxPermit) != 0;
    }
    safety.set("commands_sent", static_cast<unsigned long>(result.commands.size()));
    safety.set("ptt_set", ptt);
    safety.set("tx_permission_set", permit);
    // PTT reported by the driver from the last command it sent.
    safety.set("driver_ptt_flag", (result.end.flags & RADIOBERRY_STATS_PTT) != 0);
    safety.set("gateware_reported_tx", result.reply_tx_indicated);
    root.set("safety", safety);

    root.set("stream_clean", stream_clean(options, result));
    return root;
}

int run_app(std::span<const std::string> args, Environment& env) {
    const ParseResult parsed = parse_options(args);
    if (std::holds_alternative<ShowHelp>(parsed)) {
        env.out << kUsage;
        return kExitClean;
    }
    if (std::holds_alternative<ShowVersion>(parsed)) {
        env.out << "rbcat " << kVersion << '\n';
        return kExitClean;
    }
    if (const auto* error = std::get_if<UsageError>(&parsed)) {
        env.err << "rbcat: " << error->message << '\n' << kUsage;
        return kExitUsage;
    }
    const Options& options = std::get<Options>(parsed);
    if (options.output != Output::kNone && env.out_is_terminal) {
        env.err << "rbcat: refusing to write binary samples to a terminal\n";
        return kExitUsage;
    }

    // With samples on stdout, the report goes to stderr.
    std::ostream& report_out = options.output == Output::kNone ? env.out : env.err;
    StreamResult result;
    try {
        const std::unique_ptr<Device> device = env.open(options.device);
        result = run_stream(*device, env.clock, options, env.out, env.err, env.stop);
    } catch (const std::exception& e) {
        env.err << "rbcat: " << e.what() << '\n';
        return kExitStreamError;
    }

    const Node report = build_report(options, result);
    switch (options.format) {
        case ReportFormat::kText:
            report_out << render_text(report);
            break;
        case ReportFormat::kJson:
            report_out << render_json(report);
            break;
        case ReportFormat::kYaml:
            report_out << render_yaml(report);
            break;
    }
    return stream_clean(options, result) ? kExitClean : kExitStreamError;
}

}  // namespace radioberry::cat
