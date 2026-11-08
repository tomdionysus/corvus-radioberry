// SPDX-License-Identifier: GPL-2.0-or-later
#include "stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "radioberry/node.hpp"

namespace radioberry::cat {
namespace {

// Full scale of a 24-bit value.
constexpr double kFullScale = 8388608.0;
// Frames per read: 10 ms at 384 ksps.
constexpr std::size_t kFramesPerRead = 3840;

std::int32_t s24(const std::uint8_t* p) {
    const std::uint32_t raw =
        std::uint32_t{p[0]} << 16 | std::uint32_t{p[1]} << 8 | std::uint32_t{p[2]};
    return static_cast<std::int32_t>(raw ^ 0x800000u) - 0x800000;
}

void measure(std::span<const std::uint8_t> data, std::vector<Level>& levels) {
    const std::size_t frame = rx_frame_bytes(static_cast<unsigned>(levels.size()));
    for (std::size_t f = 0; f + frame <= data.size(); f += frame) {
        for (std::size_t r = 0; r < levels.size(); ++r) {
            const std::int32_t i = s24(&data[f + 6 * r]);
            const std::int32_t q = s24(&data[f + 6 * r + 3]);
            Level& l = levels[r];
            l.power_sum += static_cast<double>(i) * i + static_cast<double>(q) * q;
            l.samples += 1;
            l.peak = std::max({l.peak, std::abs(i), std::abs(q)});
        }
    }
}

// Top 16 bits of each 24-bit big-endian value, little-endian.
void to_iq16(std::span<const std::uint8_t> data, std::vector<char>& out) {
    out.resize(data.size() / 3 * 2);
    for (std::size_t v = 0, o = 0; v + 3 <= data.size(); v += 3, o += 2) {
        out[o] = static_cast<char>(data[v + 1]);
        out[o + 1] = static_cast<char>(data[v]);
    }
}

std::string progress_line(double elapsed, double rate, const radioberry_stats& from,
                          const radioberry_stats& to) {
    std::ostringstream line;
    line << "rbcat: " << format_double(std::round(elapsed * 10) / 10) << " s, "
         << format_double(std::round(rate * 10) / 10) << " frames/s, resyncs "
         << to.rx_resyncs - from.rx_resyncs << ", dropped words "
         << to.rx_dropped_words - from.rx_dropped_words << ", overruns "
         << to.rx_overrun_frames - from.rx_overrun_frames << '\n';
    return line.str();
}

}  // namespace

std::optional<double> Level::rms_dbfs() const {
    if (samples == 0 || power_sum <= 0) {
        return std::nullopt;
    }
    return 10 * std::log10(power_sum / static_cast<double>(samples) / (kFullScale * kFullScale));
}

std::optional<double> Level::peak_dbfs() const {
    if (peak <= 0) {
        return std::nullopt;
    }
    return 20 * std::log10(peak / kFullScale);
}

Reply send_receive_only(Device& device, const Command& command) {
    if (command.can_transmit()) {
        throw std::logic_error("rbcat refuses a command that could transmit");
    }
    return device.command(command);
}

StreamResult run_stream(Device& device, Clock& clock, const Options& options, std::ostream& samples,
                        std::ostream& progress, const std::function<bool()>& stop) {
    StreamResult result;
    const radioberry_info before = device.info();
    if (options.receivers > before.receivers) {
        throw std::runtime_error("the gateware has " + std::to_string(before.receivers) +
                                 " receivers; " + std::to_string(options.receivers) + " requested");
    }

    result.before = device.stats();
    result.commands.push_back(hl2::general(options.rate_code, options.receivers));
    for (unsigned r = 0; r < options.receivers; ++r) {
        result.commands.push_back(hl2::rx_frequency(r, options.frequencies_hz[r]));
    }
    result.commands.push_back(hl2::lna_gain(options.lna_gain_db));
    for (const Command& c : result.commands) {
        send_receive_only(device, c);
    }

    result.info = device.info();
    if (result.info.active_receivers != options.receivers ||
        result.info.rx_rate_code != options.rate_code) {
        throw std::runtime_error(
            "the driver reports " + std::to_string(result.info.active_receivers) +
            " receivers at rate code " + std::to_string(result.info.rx_rate_code) +
            " after configuring " + std::to_string(options.receivers) + " at rate code " +
            std::to_string(options.rate_code));
    }

    const std::size_t frame = rx_frame_bytes(options.receivers);
    std::vector<std::uint8_t> buffer(frame * kFramesPerRead);
    std::vector<char> converted;
    result.levels.assign(options.receivers, Level{});

    const double configured = clock.now();
    while (clock.now() - configured < options.settle_seconds) {
        if (stop()) {
            result.interrupted = true;
            result.measure_start = result.end = device.stats();
            return result;
        }
        device.read(buffer);
    }

    result.measure_start = device.stats();
    const double start = clock.now();
    double now = start;
    double next_progress = start + options.interval_seconds;
    double last_progress = start;
    std::uint64_t frames_at_progress = 0;
    radioberry_stats stats_at_progress = result.measure_start;

    while (options.seconds <= 0 || now - start < options.seconds) {
        if (stop()) {
            result.interrupted = true;
            break;
        }
        const std::size_t got = device.read(buffer);
        const std::span<const std::uint8_t> data(buffer.data(), got - got % frame);
        result.frames += data.size() / frame;
        measure(data, result.levels);

        if (options.output == Output::kIq24) {
            samples.write(reinterpret_cast<const char*>(data.data()),
                          static_cast<std::streamsize>(data.size()));
            result.bytes_out += data.size();
        } else if (options.output == Output::kIq16) {
            to_iq16(data, converted);
            samples.write(converted.data(), static_cast<std::streamsize>(converted.size()));
            result.bytes_out += converted.size();
        }
        if (!samples) {
            throw std::runtime_error("writing samples failed");
        }

        now = clock.now();
        if (options.interval_seconds > 0 && now >= next_progress) {
            const radioberry_stats s = device.stats();
            const double rate =
                static_cast<double>(result.frames - frames_at_progress) / (now - last_progress);
            progress << progress_line(now - start, rate, stats_at_progress, s) << std::flush;
            frames_at_progress = result.frames;
            stats_at_progress = s;
            last_progress = now;
            next_progress += options.interval_seconds;
        }
    }

    samples.flush();
    result.seconds = clock.now() - start;
    result.end = device.stats();
    return result;
}

}  // namespace radioberry::cat
