// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace radioberry::daemon::dsp {

// Lowers a complex stream's sample rate by an integer factor with a
// liquid-dsp FIR decimator: Kaiser low-pass at 0.4 of the output rate,
// 80 dB stopband, unity gain at DC. Factor 1 passes samples through.
class Decimator {
public:
    explicit Decimator(unsigned factor);
    ~Decimator();
    Decimator(const Decimator&) = delete;
    Decimator& operator=(const Decimator&) = delete;

    unsigned factor() const { return factor_; }
    // Takes one input sample; returns true and sets `out` when an output
    // sample is due.
    bool push(std::complex<float> in, std::complex<float>& out);
    // Clears the filter history and the partial input.
    void reset();

private:
    struct Filter;
    unsigned factor_;
    std::unique_ptr<Filter> filter_;
    std::vector<std::complex<float>> pending_;
};

}  // namespace radioberry::daemon::dsp
