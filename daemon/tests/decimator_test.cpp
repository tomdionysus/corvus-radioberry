// SPDX-License-Identifier: GPL-2.0-or-later
#include "dsp/decimator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <vector>

using radioberry::daemon::dsp::Decimator;

namespace {

// Peak output magnitude after the filter has settled, for a tone at
// `cycles_per_sample` of the input rate.
float settled_peak(Decimator& d, float cycles_per_sample, std::size_t inputs) {
    float peak = 0;
    std::size_t outputs = 0;
    for (std::size_t n = 0; n < inputs; ++n) {
        const float phase = 2.0f * 3.14159265f * cycles_per_sample * static_cast<float>(n);
        std::complex<float> y;
        if (d.push({std::cos(phase), std::sin(phase)}, y) && ++outputs > 200) {
            peak = std::max(peak, std::abs(y));
        }
    }
    return peak;
}

}  // namespace

TEST_CASE("a factor of one passes samples through", "[dsp]") {
    Decimator d(1);
    std::complex<float> y;
    REQUIRE(d.push({0.5f, -0.25f}, y));
    CHECK(y == std::complex<float>(0.5f, -0.25f));
    d.reset();
    CHECK(d.factor() == 1);
    CHECK_THROWS_AS(Decimator(0), std::invalid_argument);
}

TEST_CASE("decimation keeps unity gain in band and rejects out of band", "[dsp]") {
    for (const unsigned factor : {2u, 4u, 8u}) {
        Decimator d(factor);
        CHECK(d.factor() == factor);
        // One output per `factor` inputs.
        std::size_t outputs = 0;
        std::complex<float> y;
        for (unsigned n = 0; n < 10 * factor; ++n) {
            outputs += d.push({1.0f, 0.0f}, y) ? 1 : 0;
        }
        CHECK(outputs == 10);

        d.reset();
        // DC and a tone at a tenth of the output rate: gain 1 within 0.01 dB.
        CHECK(std::abs(settled_peak(d, 0.0f, 400 * factor) - 1.0f) < 0.0012f);
        d.reset();
        const float in_band = settled_peak(d, 0.1f / static_cast<float>(factor), 400 * factor);
        CHECK(std::abs(in_band - 1.0f) < 0.0012f);
        // A tone at 0.8 of the output rate would alias: at least 70 dB down.
        d.reset();
        const float alias = settled_peak(d, 0.8f / static_cast<float>(factor), 400 * factor);
        CHECK(20.0f * std::log10(alias) < -70.0f);
    }
}
