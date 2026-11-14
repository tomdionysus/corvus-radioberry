// SPDX-License-Identifier: GPL-2.0-or-later
#include "wspr/fano.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace wspr {

std::optional<FanoResult> fano_decode(std::span<const float, kSymbols> soft,
                                      const FanoOptions& options) {
    // Per coded bit, the metric of hearing it as 0 and as 1: the bit's
    // log-likelihood against the average of both, less the code rate.
    std::array<std::array<float, 2>, kSymbols> metric{};
    float rms = 0;
    for (const float y : soft) {
        rms += y * y;
    }
    rms = std::sqrt(rms / kSymbols);
    if (rms <= 0) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kSymbols; ++i) {
        const float x = options.llr_scale * soft[i] / rms;
        metric[i][1] =
            static_cast<float>(1.0 - std::log2(1.0 + std::exp(-static_cast<double>(x))) - 0.5);
        metric[i][0] =
            static_cast<float>(1.0 - std::log2(1.0 + std::exp(static_cast<double>(x))) - 0.5);
    }

    std::array<std::uint32_t, kCodedBits + 1> state{};
    std::array<float, kCodedBits + 1> gamma{};
    std::array<std::array<float, 2>, kCodedBits> branch{};
    std::array<std::array<std::uint8_t, 2>, kCodedBits> order{};
    std::array<std::uint8_t, kCodedBits> tried{};
    std::array<std::uint8_t, kCodedBits> bits{};

    const auto prepare = [&](int k) {
        const auto i = static_cast<std::size_t>(k);
        for (std::uint32_t b = 0; b < 2; ++b) {
            const std::uint32_t reg = state[i] << 1 | b;
            branch[i][b] =
                metric[2 * i][parity(reg & kPoly1)] + metric[2 * i + 1][parity(reg & kPoly2)];
        }
        if (k >= kDataBits) {
            order[i] = {0, 0};  // the tail is all zeros
        } else if (branch[i][1] > branch[i][0]) {
            order[i] = {1, 0};
        } else {
            order[i] = {0, 1};
        }
        tried[i] = 0;
    };

    const float delta = options.delta;
    const long limit = options.max_cycles_per_bit * kCodedBits;
    float threshold = 0;
    int k = 0;
    prepare(0);
    for (long cycle = 0; cycle < limit; ++cycle) {
        const auto i = static_cast<std::size_t>(k);
        const std::uint8_t b = order[i][tried[i]];
        const float g = gamma[i] + branch[i][b];
        if (g >= threshold) {
            // First visit to the next node: tighten the threshold.
            if (gamma[i] < threshold + delta) {
                while (g >= threshold + delta) {
                    threshold += delta;
                }
            }
            bits[i] = b;
            state[i + 1] = state[i] << 1 | b;
            gamma[i + 1] = g;
            ++k;
            if (k == kCodedBits) {
                FanoResult result;
                for (int n = 0; n < kDataBits; ++n) {
                    result.payload[static_cast<std::size_t>(n / 8)] |=
                        static_cast<std::uint8_t>(bits[static_cast<std::size_t>(n)] << (7 - n % 8));
                }
                result.metric = g;
                result.cycles = cycle + 1;
                return result;
            }
            prepare(k);
            continue;
        }
        // Cannot go forward: back up to a node with a branch left to try,
        // or lower the threshold where backing up is not allowed.
        for (;;) {
            if (k == 0 || gamma[static_cast<std::size_t>(k - 1)] < threshold) {
                threshold -= delta;
                tried[static_cast<std::size_t>(k)] = 0;
                break;
            }
            --k;
            const auto j = static_cast<std::size_t>(k);
            if (k < kDataBits && tried[j] == 0) {
                tried[j] = 1;
                break;
            }
        }
    }
    return std::nullopt;
}

}  // namespace wspr
