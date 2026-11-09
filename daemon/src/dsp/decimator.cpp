// SPDX-License-Identifier: GPL-2.0-or-later
#include "dsp/decimator.hpp"

// liquid.h uses std::complex only when <complex> came first.
// clang-format off
#include <complex>
#include <liquid/liquid.h>
// clang-format on

#include <stdexcept>

namespace radioberry::daemon::dsp {
namespace {

constexpr unsigned kTapsPerFactor = 24;  // filter length 24 * factor + 1
constexpr float kStopbandDb = 80.0f;
constexpr float kPassFraction = 0.4f;  // of the output rate

}  // namespace

struct Decimator::Filter {
    firdecim_crcf q = nullptr;
    ~Filter() {
        if (q != nullptr) {
            firdecim_crcf_destroy(q);
        }
    }
};

Decimator::Decimator(unsigned factor) : factor_(factor), filter_(std::make_unique<Filter>()) {
    if (factor == 0) {
        throw std::invalid_argument("decimation factor 0");
    }
    if (factor > 1) {
        const unsigned taps = kTapsPerFactor * factor + 1;
        std::vector<float> h(taps);
        liquid_firdes_kaiser(taps, kPassFraction / static_cast<float>(factor), kStopbandDb, 0.0f,
                             h.data());
        float sum = 0;
        for (const float t : h) {
            sum += t;
        }
        for (float& t : h) {
            t /= sum;
        }
        filter_->q = firdecim_crcf_create(factor, h.data(), taps);
        pending_.reserve(factor);
    }
}

Decimator::~Decimator() = default;

bool Decimator::push(std::complex<float> in, std::complex<float>& out) {
    if (factor_ == 1) {
        out = in;
        return true;
    }
    pending_.push_back(in);
    if (pending_.size() < factor_) {
        return false;
    }
    firdecim_crcf_execute(filter_->q, pending_.data(), &out);
    pending_.clear();
    return true;
}

void Decimator::reset() {
    pending_.clear();
    if (filter_->q != nullptr) {
        firdecim_crcf_reset(filter_->q);
    }
}

}  // namespace radioberry::daemon::dsp
