// SPDX-License-Identifier: GPL-2.0-or-later
#include "options.hpp"

#include <charconv>
#include <cmath>
#include <optional>

#include "radioberry/command.hpp"

namespace radioberry::cat {

const char* const kUsage =
    "usage: rbcat [options]\n"
    "\n"
    "Receives from /dev/radioberry: sets the sample rate, receiver count,\n"
    "frequencies and LNA gain, streams RX frames to stdout or discards them,\n"
    "and reports frame rate, signal level and the driver's statistics. Never\n"
    "sets PTT or the TX permission bit. Exits 0 when the stream was clean, 1\n"
    "on stream errors or a device failure, 2 on a usage error.\n"
    "\n"
    "  --rate KSPS          48, 96, 192 or 384 (default 48)\n"
    "  --receivers N        active receivers (default 1)\n"
    "  --freq HZ[,HZ...]    receiver frequencies; one value sets all\n"
    "                       (default 14074000)\n"
    "  --lna DB             LNA gain, -12 to 48 (default 20)\n"
    "  --output FORMAT      none (default), iq24 (frames as read: 24-bit\n"
    "                       big-endian I and Q per receiver) or iq16 (top 16\n"
    "                       bits of each value, little-endian); samples go to\n"
    "                       stdout and the report to stderr\n"
    "  --seconds S          measure for S seconds (default: until SIGINT or\n"
    "                       SIGTERM)\n"
    "  --settle S           discard S seconds after configuring (default 0.5)\n"
    "  --interval S         progress line on stderr every S seconds\n"
    "  --device PATH        default /dev/radioberry\n"
    "  --json               JSON report\n"
    "  --yaml               YAML report\n"
    "  --version            print the version\n"
    "  --help               this text\n"
    "\n"
    "The PIO bus carries about 2.37 million values per second: six receivers\n"
    "at 192 ksps or three at 384 ksps. Above that the stream drops data.\n";

namespace {

template <typename T>
std::optional<T> parse_number(const std::string& text) {
    T value{};
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc() || ptr != end || text.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parse_seconds(const std::string& text) {
    const auto value = parse_number<double>(text);
    if (!value || !std::isfinite(*value) || *value < 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<std::uint32_t>> parse_frequencies(const std::string& text) {
    std::vector<std::uint32_t> result;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = text.find(',', start);
        const auto hz = parse_number<std::uint32_t>(text.substr(start, comma - start));
        if (!hz || *hz > kMaxFrequencyHz) {
            return std::nullopt;
        }
        result.push_back(*hz);
        if (comma == std::string::npos) {
            return result;
        }
        start = comma + 1;
    }
}

}  // namespace

std::string to_string(Output output) {
    switch (output) {
        case Output::kIq24:
            return "iq24";
        case Output::kIq16:
            return "iq16";
        case Output::kNone:
            break;
    }
    return "none";
}

ParseResult parse_options(std::span<const std::string> args) {
    Options o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto value = [&]() -> const std::string* {
            return i + 1 < args.size() ? &args[++i] : nullptr;
        };
        auto bad = [&](const std::string& what) { return UsageError{arg + " needs " + what}; };

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
        } else if (arg == "--rate") {
            const std::string* v = value();
            const auto ksps = v ? parse_number<unsigned>(*v) : std::nullopt;
            const auto code = ksps ? rate_code_for_ksps(*ksps) : std::nullopt;
            if (!code) {
                return bad("48, 96, 192 or 384");
            }
            o.rate_ksps = *ksps;
            o.rate_code = *code;
        } else if (arg == "--receivers") {
            const std::string* v = value();
            const auto n = v ? parse_number<unsigned>(*v) : std::nullopt;
            if (!n || *n < 1 || *n > 12) {
                return bad("a receiver count from 1 to 12");
            }
            o.receivers = *n;
        } else if (arg == "--freq") {
            const std::string* v = value();
            const auto f = v ? parse_frequencies(*v) : std::nullopt;
            if (!f) {
                return bad("frequencies in Hz from 0 to 38400000, separated by commas");
            }
            o.frequencies_hz = *f;
        } else if (arg == "--lna") {
            const std::string* v = value();
            const auto db = v ? parse_number<int>(*v) : std::nullopt;
            if (!db || *db < kLnaGainMinDb || *db > kLnaGainMaxDb) {
                return bad("a gain in dB from -12 to 48");
            }
            o.lna_gain_db = *db;
        } else if (arg == "--output") {
            const std::string* v = value();
            if (v && *v == "none") {
                o.output = Output::kNone;
            } else if (v && *v == "iq24") {
                o.output = Output::kIq24;
            } else if (v && *v == "iq16") {
                o.output = Output::kIq16;
            } else {
                return bad("none, iq24 or iq16");
            }
        } else if (arg == "--seconds" || arg == "--settle" || arg == "--interval") {
            const std::string* v = value();
            const auto s = v ? parse_seconds(*v) : std::nullopt;
            if (!s) {
                return bad("a number of seconds");
            }
            (arg == "--seconds"  ? o.seconds
             : arg == "--settle" ? o.settle_seconds
                                 : o.interval_seconds) = *s;
        } else if (arg == "--device") {
            const std::string* v = value();
            if (!v) {
                return bad("a path");
            }
            o.device = *v;
        } else {
            return UsageError{"unknown option " + arg};
        }
    }

    if (o.frequencies_hz.size() == 1) {
        o.frequencies_hz.assign(o.receivers, o.frequencies_hz.front());
    } else if (o.frequencies_hz.size() != o.receivers) {
        return UsageError{"--freq gives " + std::to_string(o.frequencies_hz.size()) +
                          " frequencies for " + std::to_string(o.receivers) + " receivers"};
    }
    return o;
}

}  // namespace radioberry::cat
