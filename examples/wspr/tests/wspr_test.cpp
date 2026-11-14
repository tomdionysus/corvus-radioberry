// SPDX-License-Identifier: GPL-2.0-or-later
//
// The WSPR chain without a radio. The packed messages and channel symbols
// are wsprsim's (WSJT-X 2.7.0) for the same messages; the signals are
// synthesised in noise and decoded as wsprrx does.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "wspr/codec.hpp"
#include "wspr/decimator.hpp"
#include "wspr/decoder.hpp"
#include "wspr/fano.hpp"

using namespace wspr;

namespace {

Payload bytes(std::initializer_list<int> b) {
    Payload p{};
    std::size_t i = 0;
    for (const int v : b) {
        p[i++] = static_cast<std::uint8_t>(v);
    }
    return p;
}

// wsprsim -c "K1ABC FN42 33"
const Symbols kK1abcSymbols = {
    3, 3, 0, 2, 2, 2, 0, 0, 1, 0, 2, 0, 1, 1, 1, 0, 2, 2, 1, 2, 0, 1, 2, 1, 1, 1, 3,
    0, 2, 2, 2, 2, 0, 0, 3, 2, 0, 1, 2, 3, 2, 0, 0, 0, 2, 0, 3, 0, 1, 1, 0, 2, 3, 1,
    2, 3, 0, 2, 2, 3, 3, 2, 1, 2, 2, 2, 0, 3, 3, 0, 3, 2, 3, 2, 1, 0, 1, 0, 2, 3, 2,
    0, 3, 2, 1, 3, 2, 0, 0, 1, 3, 0, 3, 2, 3, 2, 2, 0, 3, 0, 2, 2, 2, 0, 1, 0, 2, 3,
    0, 2, 1, 3, 1, 2, 3, 3, 0, 2, 3, 1, 2, 1, 2, 2, 2, 3, 3, 3, 2, 0, 0, 0, 0, 1, 0,
    3, 2, 0, 1, 3, 2, 2, 2, 0, 2, 0, 2, 3, 3, 2, 3, 2, 3, 3, 2, 2, 0, 1, 1, 2, 2, 2};

// A slot at `rate`: each message starting kStartOffset + dt into it at
// `hz` from the centre, unit amplitude, in complex Gaussian noise set for
// `snr_db` in 2500 Hz.
std::vector<std::complex<float>> slot(const std::vector<std::pair<std::string, float>>& messages,
                                      float dt, float snr_db, float rate, unsigned seed = 7) {
    const auto n = static_cast<std::size_t>(120.0f * rate);
    std::vector<std::complex<float>> x(n);
    // Noise power in 2500 Hz is 10^(-snr/10); per component over `rate`.
    const float sigma = std::sqrt(std::pow(10.0f, -snr_db / 10.0f) * rate / 2500.0f / 2.0f);
    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0.0f, sigma);
    for (auto& s : x) {
        s = {gauss(rng), gauss(rng)};
    }
    for (const auto& [text, hz] : messages) {
        const auto p = pack(text);
        REQUIRE(p);
        const auto signal = synthesize(encode(*p), hz, rate);
        const auto start = static_cast<std::size_t>((kStartOffset + dt) * rate);
        for (std::size_t i = 0; i < signal.size() && start + i < n; ++i) {
            x[start + i] += signal[i];
        }
    }
    return x;
}

}  // namespace

TEST_CASE("type 1 messages pack as wsprsim packs them", "[wspr]") {
    CHECK(pack("K1ABC FN42 33") == bytes({0xf7, 0x0c, 0x23, 0x8b, 0x0d, 0x18, 0x40}));
    CHECK(pack("G4ABC IO91 10") == bytes({0xf6, 0x5a, 0x56, 0x97, 0xfa, 0xb2, 0x80}));
    CHECK(pack("W1A AA00 0") == bytes({0xf9, 0x4c, 0xf5, 0xbf, 0xbb, 0x90, 0x00}));
    CHECK_FALSE(pack("K1ABC FN42 34"));  // power ends in 0, 3 or 7
    CHECK_FALSE(pack("K1ABC ZZ42 33"));
    CHECK_FALSE(pack("ABCDEF FN42 33"));
    CHECK_FALSE(pack("PJ4/K1ABC 33"));
}

TEST_CASE("channel symbols match wsprsim's", "[wspr]") {
    const auto p = pack("K1ABC FN42 33");
    REQUIRE(p);
    CHECK(encode(*p) == kK1abcSymbols);
}

TEST_CASE("all three message types unpack", "[wspr]") {
    CallHashes hashes;
    CHECK(unpack(bytes({0xf7, 0x0c, 0x23, 0x8b, 0x0d, 0x18, 0x40}), hashes) == "K1ABC FN42 33");
    CHECK(unpack(bytes({0xf9, 0x4c, 0xf5, 0xbf, 0xbb, 0x90, 0x00}), hashes) == "W1A AA00 0");
    // Type 3 before its call has been heard, then after.
    const Payload type3 = bytes({0x88, 0x24, 0x7c, 0x69, 0xa2, 0xe7, 0x80});
    CHECK(unpack(type3, hashes) == "<...> FK52UD 33");
    CHECK(unpack(bytes({0xf7, 0x0c, 0x23, 0x81, 0x0e, 0x98, 0xc0}), hashes) == "PJ4/K1ABC 33");
    CHECK(unpack(type3, hashes) == "<PJ4/K1ABC> FK52UD 33");
    CHECK(unpack(bytes({0xf7, 0x0c, 0x23, 0x8d, 0x4c, 0xf9, 0xc0}), hashes) == "K1ABC/7 37");
    CHECK(unpack(bytes({0xf7, 0x0c, 0x23, 0x8d, 0x4f, 0x39, 0xc0}), hashes) == "K1ABC/P 37");
    CHECK(unpack(bytes({0xf7, 0x0c, 0x23, 0x8d, 0x50, 0xd9, 0xc0}), hashes) == "K1ABC/12 37");
    CHECK(unpack(bytes({0xf6, 0x5a, 0x56, 0x95, 0x15, 0xba, 0x80}), hashes) == "VK2/G4ABC 40");
    CHECK(call_hash("PJ4/K1ABC") == 19735);
}

TEST_CASE("the Fano decoder recovers a payload from noisy soft bits", "[wspr]") {
    const auto p = pack("G4ABC IO91 10");
    REQUIRE(p);
    const Symbols s = encode(*p);
    std::array<float, kSymbols> soft{};
    std::mt19937 rng(3);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    for (std::size_t i = 0; i < kSymbols; ++i) {
        // Eb/N0 about 4 dB, at which most frames decode.
        soft[i] = (s[i] >> 1 ? 1.1f : -1.1f) + gauss(rng);
    }
    const auto result = fano_decode(deinterleave(soft));
    REQUIRE(result);
    CHECK(result->payload == *p);
}

TEST_CASE("the decimator passes WSPR's band and stops what would alias into it", "[wspr]") {
    const auto level = [](float hz) {
        std::vector<std::complex<float>> tone(96000);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            const double phase = 2 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0;
            tone[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        }
        Decimator d;
        std::vector<std::complex<float>> out;
        d.process(tone, out);
        double power = 0;
        for (std::size_t i = out.size() / 2; i < out.size(); ++i) {
            power += std::norm(out[i]);
        }
        return 10 * std::log10(power / static_cast<double>(out.size() - out.size() / 2));
    };
    CHECK(level(0) == Catch::Approx(0).margin(0.5));
    CHECK(level(110) == Catch::Approx(0).margin(0.5));
    CHECK(level(-110) == Catch::Approx(0).margin(0.5));
    CHECK(level(270) < -60);  // would alias to -105 Hz
    CHECK(level(-300) < -60);
    CHECK(level(5000) < -60);
}

TEST_CASE("a weak signal decodes with its frequency, time and SNR", "[wspr]") {
    Decoder decoder;
    const auto decodes = decoder.decode(slot({{"K1ABC FN42 33", 37.3f}}, 0.4f, -24.0f, 375.0f));
    REQUIRE(decodes.size() == 1);
    CHECK(decodes[0].text == "K1ABC FN42 33");
    CHECK(decodes[0].freq_hz == Catch::Approx(37.3f).margin(0.2f));
    CHECK(decodes[0].dt_s == Catch::Approx(0.4f).margin(0.1f));
    CHECK(decodes[0].snr_db == Catch::Approx(-24.0f).margin(2.0f));
    CHECK(decodes[0].drift_hz == Catch::Approx(0.0f).margin(0.6f));
}

TEST_CASE("several signals across the band decode in one slot", "[wspr]") {
    Decoder decoder;
    const auto decodes = decoder.decode(
        slot({{"K1ABC FN42 33", -80.0f}, {"G4ABC IO91 10", 5.0f}, {"W1A AA00 0", 90.0f}}, 0.0f,
             -20.0f, 375.0f));
    std::vector<std::string> texts;
    for (const auto& d : decodes) {
        texts.push_back(d.text);
    }
    CHECK(texts == std::vector<std::string>{"K1ABC FN42 33", "G4ABC IO91 10", "W1A AA00 0"});
}

TEST_CASE("noise alone decodes nothing", "[wspr]") {
    Decoder decoder;
    for (unsigned seed = 1; seed <= 3; ++seed) {
        CHECK(decoder.decode(slot({}, 0.0f, -20.0f, 375.0f, seed)).empty());
    }
}

TEST_CASE("WSPR at 48 ksps decodes through the decimator", "[wspr]") {
    const auto wide = slot({{"G4ABC IO91 10", -50.0f}}, 1.0f, -22.0f, 48000.0f);
    Decimator decimator;
    std::vector<std::complex<float>> narrow;
    // In packet-sized pieces, as wsprrx feeds it.
    for (std::size_t i = 0; i < wide.size(); i += 126) {
        const std::size_t n = std::min<std::size_t>(126, wide.size() - i);
        decimator.process(std::span(wide).subspan(i, n), narrow);
    }
    Decoder decoder;
    const auto decodes = decoder.decode(narrow);
    REQUIRE(decodes.size() == 1);
    CHECK(decodes[0].text == "G4ABC IO91 10");
    CHECK(decodes[0].freq_hz == Catch::Approx(-50.0f).margin(0.2f));
    // The decimating filter delays by half its length: 1024 samples.
    CHECK(decodes[0].dt_s == Catch::Approx(1.0f + 1024.0f / 48000.0f).margin(0.1f));
}
