// SPDX-License-Identifier: GPL-2.0-or-later
#include "wspr/decimator.hpp"

#include <vector>

namespace wspr {
namespace {

constexpr unsigned kTaps = Decimator::kFactor * 16 + 1;
constexpr float kCutoffHz = Decimator::kOutputRate / 2;
constexpr float kStopbandDb = 70.0f;

}  // namespace

Decimator::Decimator() {
    std::vector<float> taps(kTaps);
    liquid_firdes_kaiser(kTaps, kCutoffHz / kInputRate, kStopbandDb, 0.0f, taps.data());
    float sum = 0;
    for (const float t : taps) {
        sum += t;
    }
    for (float& t : taps) {
        t /= sum;
    }
    filter_.reset(firdecim_crcf_create(kFactor, taps.data(), kTaps));
    pending_.reserve(kFactor);
}

void Decimator::process(std::span<const std::complex<float>> input,
                        std::vector<std::complex<float>>& out) {
    for (const std::complex<float>& x : input) {
        pending_.push_back(x);
        if (pending_.size() == kFactor) {
            std::complex<float> y;
            firdecim_crcf_execute(filter_.get(), pending_.data(), &y);
            out.push_back(y);
            pending_.clear();
        }
    }
}

}  // namespace wspr
