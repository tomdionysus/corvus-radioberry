// SPDX-License-Identifier: GPL-2.0-or-later
// clang-format off
#include <complex>
#include <liquid/liquid.h>
// clang-format on

#include "wspr/decoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace wspr {
namespace {

using cf = std::complex<float>;

// Spectrogram: one-symbol windows, zero-padded to give half-tone bins, in
// half-symbol steps.
constexpr std::size_t kFft = std::size_t{2} * kSymbolSamples;
constexpr float kBinHz = kSampleRate / kFft;  // 0.732 Hz, half a tone spacing
constexpr std::size_t kStep = kSymbolSamples / 2;
constexpr std::size_t kSteps = (kSlotSamples - kSymbolSamples) / kStep + 1;
constexpr int kMaxBin = 220;  // +-161 Hz kept
constexpr int kBins = 2 * kMaxBin + 1;
constexpr int kMaxLagSteps = static_cast<int>(kSteps) - 2 * (kSymbols - 1) - 1;
constexpr std::array<int, 4> kToneBins = {-3, -1, 1, 3};

// Index of bin `b` (-kMaxBin..kMaxBin) in a per-bin vector.
std::size_t bin_index(int b) {
    const int i = b + kMaxBin;
    return static_cast<std::size_t>(i);
}

struct Spectrogram {
    std::vector<float> power;  // [step][bin + kMaxBin]
    float at(std::size_t step, int bin) const {
        return power[step * kBins + static_cast<std::size_t>(bin + kMaxBin)];
    }
};

Spectrogram spectrogram(const std::vector<cf>& x) {
    Spectrogram s;
    s.power.resize(kSteps * kBins);
    std::vector<cf> in(kFft), out(kFft);
    fftplan plan =
        fft_create_plan(static_cast<unsigned>(kFft), in.data(), out.data(), LIQUID_FFT_FORWARD, 0);
    for (std::size_t t = 0; t < kSteps; ++t) {
        std::fill(in.begin(), in.end(), cf{});
        std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(t * kStep), kSymbolSamples, in.begin());
        fft_execute(plan);
        for (int b = -kMaxBin; b <= kMaxBin; ++b) {
            const auto i =
                static_cast<std::size_t>((b + static_cast<int>(kFft)) % static_cast<int>(kFft));
            s.power[t * kBins + static_cast<std::size_t>(b + kMaxBin)] = std::norm(out[i]);
        }
    }
    fft_destroy_plan(plan);
    return s;
}

float drift_offset(float drift_hz, std::size_t k) {
    return drift_hz * (static_cast<float>(k) - (kSymbols - 1) / 2.0f) / kSymbols;
}

// Normalised sync correlation from the four tone powers of each symbol.
float sync_metric(const std::array<std::array<float, 4>, kSymbols>& p) {
    float num = 0, den = 0;
    for (std::size_t k = 0; k < kSymbols; ++k) {
        const float s = kSync[k] ? 1.0f : -1.0f;
        num += s * ((p[k][1] + p[k][3]) - (p[k][0] + p[k][2]));
        den += p[k][0] + p[k][1] + p[k][2] + p[k][3];
    }
    return den > 0 ? num / den : 0;
}

struct Fit {
    long start = 0;  // sample where the first symbol starts
    float freq_hz = 0;
    float drift_hz = 0;
    float sync = -1;
};

// Each symbol's tone powers, measured by correlating against the four
// tones over the symbol (samples outside the slot count as zero).
void tone_powers(const std::vector<cf>& x, const Fit& f,
                 std::array<std::array<float, 4>, kSymbols>& p) {
    const auto n = static_cast<long>(x.size());
    for (std::size_t k = 0; k < kSymbols; ++k) {
        const long t0 = f.start + static_cast<long>(k) * kSymbolSamples;
        const float fk = f.freq_hz + drift_offset(f.drift_hz, k);
        for (std::size_t j = 0; j < 4; ++j) {
            const double hz = fk + (static_cast<double>(j) - 1.5) * kToneSpacing;
            const double w = -2 * std::numbers::pi * hz / kSampleRate;
            const cf step(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
            cf rot(1, 0), acc(0, 0);
            for (long i = 0; i < kSymbolSamples; ++i) {
                const long t = t0 + i;
                if (t >= 0 && t < n) {
                    acc += x[static_cast<std::size_t>(t)] * rot;
                }
                rot *= step;
            }
            p[k][j] = std::norm(acc);
        }
    }
}

float evaluate(const std::vector<cf>& x, const Fit& f) {
    std::array<std::array<float, 4>, kSymbols> p{};
    tone_powers(x, f, p);
    return sync_metric(p);
}

// Refines start, frequency and drift by a coordinate search around `f`.
Fit refine(const std::vector<cf>& x, Fit f) {
    f.sync = evaluate(x, f);
    const auto try_fit = [&](Fit c) {
        c.sync = evaluate(x, c);
        if (c.sync > f.sync) {
            f = c;
        }
    };
    const Fit coarse = f;
    for (long dt = -64; dt <= 64; dt += 16) {
        if (dt != 0) {
            try_fit(Fit{coarse.start + dt, f.freq_hz, f.drift_hz, 0});
        }
    }
    for (const float df : {-0.36f, -0.24f, -0.12f, 0.12f, 0.24f, 0.36f}) {
        try_fit(Fit{f.start, coarse.freq_hz + df, f.drift_hz, 0});
    }
    for (const float dd : {-0.5f, 0.5f}) {
        try_fit(Fit{f.start, f.freq_hz, coarse.drift_hz + dd, 0});
    }
    const Fit mid = f;
    for (long dt = -8; dt <= 8; dt += 4) {
        if (dt != 0) {
            try_fit(Fit{mid.start + dt, f.freq_hz, f.drift_hz, 0});
        }
    }
    for (const float df : {-0.06f, -0.03f, 0.03f, 0.06f}) {
        try_fit(Fit{f.start, mid.freq_hz + df, f.drift_hz, 0});
    }
    return f;
}

struct Candidate {
    int bin = 0;
    float power = 0;
};

}  // namespace

std::vector<Decoder::Decode> Decoder::decode(std::span<const std::complex<float>> slot) {
    std::vector<cf> x(kSlotSamples);
    std::copy_n(slot.begin(), std::min(slot.size(), kSlotSamples), x.begin());
    const Spectrogram s = spectrogram(x);

    // Average spectrum, its noise floor, and peaks of its sum over a
    // transmission's width as candidates.
    std::vector<float> avg(kBins);
    for (std::size_t t = 0; t < kSteps; ++t) {
        for (int b = -kMaxBin; b <= kMaxBin; ++b) {
            avg[bin_index(b)] += s.at(t, b);
        }
    }
    std::vector<float> sorted = avg;
    std::nth_element(sorted.begin(), sorted.begin() + kBins * 3 / 10, sorted.end());
    const float floor = sorted[kBins * 3 / 10];
    if (floor <= 0) {
        return {};
    }
    const int lo = static_cast<int>(std::ceil(options_.min_hz / kBinHz));
    const int hi = static_cast<int>(std::floor(options_.max_hz / kBinHz));
    std::vector<float> smooth(kBins);
    for (int b = -kMaxBin + 3; b <= kMaxBin - 3; ++b) {
        float sum = 0;
        for (int j = -3; j <= 3; ++j) {
            sum += avg[bin_index(b + j)];
        }
        smooth[bin_index(b)] = sum / (7 * floor);
    }
    std::vector<Candidate> candidates;
    for (int b = std::max(lo, -kMaxBin + 12); b <= std::min(hi, kMaxBin - 12); ++b) {
        const float v = smooth[bin_index(b)];
        if (v > 1.02f && v >= smooth[bin_index(b - 1)] && v > smooth[bin_index(b + 1)]) {
            candidates.push_back({b, v});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.power > b.power; });
    if (candidates.size() > options_.max_candidates) {
        candidates.resize(options_.max_candidates);
    }

    std::vector<Decode> decodes;
    for (const Candidate& c : candidates) {
        // Coarse sync over start (in half-symbol steps), frequency (in half
        // tones) and drift, on the spectrogram.
        Fit best;
        for (int drift = -4; drift <= 4; ++drift) {
            for (int db = -2; db <= 2; ++db) {
                for (int lag = 0; lag <= kMaxLagSteps; ++lag) {
                    float num = 0, den = 0;
                    for (std::size_t k = 0; k < kSymbols; ++k) {
                        const int centre =
                            c.bin + db +
                            static_cast<int>(
                                std::lround(drift_offset(static_cast<float>(drift), k) / kBinHz));
                        const std::size_t t = static_cast<std::size_t>(lag) + 2 * k;
                        std::array<float, 4> p{};
                        for (std::size_t j = 0; j < 4; ++j) {
                            p[j] = s.at(t, centre + kToneBins[j]);
                        }
                        const float sg = kSync[k] ? 1.0f : -1.0f;
                        num += sg * ((p[1] + p[3]) - (p[0] + p[2]));
                        den += p[0] + p[1] + p[2] + p[3];
                    }
                    const float m = den > 0 ? num / den : 0;
                    if (m > best.sync) {
                        best = Fit{static_cast<long>(lag) * static_cast<long>(kStep),
                                   static_cast<float>(c.bin + db) * kBinHz,
                                   static_cast<float>(drift), m};
                    }
                }
            }
        }
        if (best.sync < options_.min_sync) {
            continue;
        }
        const Fit fit = refine(x, best);

        // Soft data bits: the louder of the two tones the sync bit allows.
        std::array<std::array<float, 4>, kSymbols> p{};
        tone_powers(x, fit, p);
        std::array<float, kSymbols> soft{};
        for (std::size_t k = 0; k < kSymbols; ++k) {
            const std::size_t sb = kSync[k];
            soft[k] = std::sqrt(p[k][2 + sb]) - std::sqrt(p[k][sb]);
        }
        const auto coded = deinterleave(soft);
        const auto result = fano_decode(coded, options_.fano);
        if (!result) {
            continue;
        }
        const auto text = unpack(result->payload, hashes_);
        if (!text || std::any_of(decodes.begin(), decodes.end(),
                                 [&](const Decode& d) { return d.text == *text; })) {
            continue;
        }

        // SNR from the tones sent against the three not sent, scaled from
        // one tone's bandwidth to 2500 Hz.
        const Symbols sent = encode(result->payload);
        double signal = 0, noise = 0;
        for (std::size_t k = 0; k < kSymbols; ++k) {
            for (std::size_t j = 0; j < 4; ++j) {
                (j == sent[k] ? signal : noise) += p[k][j];
            }
        }
        signal /= kSymbols;
        noise /= 3.0 * kSymbols;
        const double ratio = noise > 0 ? (signal - noise) / noise : 0;
        const float snr =
            ratio > 0 ? static_cast<float>(10 * std::log10(ratio * kToneSpacing / 2500.0)) : -40.0f;

        Decode d;
        d.text = *text;
        d.snr_db = std::max(snr, -40.0f);
        d.dt_s = static_cast<float>(fit.start) / kSampleRate - kStartOffset;
        d.freq_hz = fit.freq_hz;
        d.drift_hz = fit.drift_hz;
        decodes.push_back(d);
    }
    std::sort(decodes.begin(), decodes.end(),
              [](const Decode& a, const Decode& b) { return a.freq_hz < b.freq_hz; });
    return decodes;
}

}  // namespace wspr
