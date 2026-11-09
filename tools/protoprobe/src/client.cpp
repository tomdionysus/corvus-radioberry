// SPDX-License-Identifier: GPL-2.0-or-later
#include "client.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace radioberry::proto {
namespace {

constexpr double kFullScale = 8388608.0;  // 2^23

double dbfs(double amplitude) {
    if (amplitude <= 0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 20.0 * std::log10(amplitude / kFullScale);
}

}  // namespace

void Levels::add(std::int32_t i, std::int32_t q) {
    ++samples_;
    sum_squares_ += static_cast<double>(i) * i + static_cast<double>(q) * q;
    peak_ = std::max({peak_, std::abs(i), std::abs(q)});
}

double Levels::rms_dbfs() const {
    return samples_ == 0 ? dbfs(0) : dbfs(std::sqrt(sum_squares_ / static_cast<double>(samples_)));
}

double Levels::peak_dbfs() const { return dbfs(peak_); }

void Sequence::add(std::uint32_t sequence) {
    if (last_ && sequence != *last_ + 1) {
        ++breaks_;
        // Ahead: the packets in between are lost. Behind or repeated: a
        // reorder or a restart, counted as a break only.
        const std::uint32_t ahead = sequence - *last_ - 1;
        if (ahead < 0x80000000u) {
            lost_ += ahead;
        }
    }
    last_ = sequence;
}

void Client::reset_counts() {
    const std::size_t receivers = counts_.levels.size();
    counts_ = StreamCounts{};
    counts_.levels.resize(receivers);
    for (Sequence& s : sequences_) {
        s.reset_counts();
    }
}

void Client::count_sequence(std::size_t stream, std::uint32_t sequence) {
    if (sequences_.size() <= stream) {
        sequences_.resize(stream + 1);
    }
    sequences_[stream].add(sequence);
    counts_.sequence_breaks = 0;
    counts_.lost_packets = 0;
    for (const Sequence& s : sequences_) {
        counts_.sequence_breaks += s.breaks();
        counts_.lost_packets += s.lost();
    }
}

std::uint32_t be32(std::span<const std::uint8_t> p, std::size_t at) {
    return std::uint32_t{p[at]} << 24 | std::uint32_t{p[at + 1]} << 16 |
           std::uint32_t{p[at + 2]} << 8 | std::uint32_t{p[at + 3]};
}

void put_be32(std::span<std::uint8_t> p, std::size_t at, std::uint32_t v) {
    p[at] = static_cast<std::uint8_t>(v >> 24);
    p[at + 1] = static_cast<std::uint8_t>(v >> 16);
    p[at + 2] = static_cast<std::uint8_t>(v >> 8);
    p[at + 3] = static_cast<std::uint8_t>(v);
}

std::int32_t s24(std::span<const std::uint8_t> p, std::size_t at) {
    std::int32_t v = std::int32_t{p[at]} << 16 | std::int32_t{p[at + 1]} << 8 | p[at + 2];
    if ((v & 0x800000) != 0) {
        v -= 1 << 24;
    }
    return v;
}

}  // namespace radioberry::proto
