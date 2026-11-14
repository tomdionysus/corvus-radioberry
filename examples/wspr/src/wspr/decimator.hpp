// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// clang-format off
#include <complex>
#include <liquid/liquid.h>
// clang-format on

#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace wspr {

// 48 ksps complex baseband down to the decoder's 375 sps: a unity-gain
// Kaiser low-pass (2049 taps, 70 dB stopband) at the output Nyquist, 187.5
// Hz, and every 128th sample, through liquid-dsp. WSPR's 200 Hz, centred,
// is inside the passband, and what aliases into it is in the stopband.
// Stateful: a stream can be fed in pieces of any size.
class Decimator {
public:
    static constexpr unsigned kFactor = 128;
    static constexpr float kInputRate = 48000.0f;
    static constexpr float kOutputRate = kInputRate / kFactor;

    Decimator();

    // Appends the output for `input` to `out`.
    void process(std::span<const std::complex<float>> input, std::vector<std::complex<float>>& out);

private:
    struct Destroy {
        void operator()(firdecim_crcf q) const { firdecim_crcf_destroy(q); }
    };
    std::unique_ptr<std::remove_pointer_t<firdecim_crcf>, Destroy> filter_;
    std::vector<std::complex<float>> pending_;
};

}  // namespace wspr
