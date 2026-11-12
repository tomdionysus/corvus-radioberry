// SPDX-License-Identifier: GPL-2.0-or-later
//
// WSPR decoding of one two-minute slot of complex baseband at 375 sps,
// centred 1500 Hz above the dial: candidates from the average spectrum, a
// sync search over time, frequency and drift, a finer search with the
// tones measured directly, soft symbols and Fano decoding.
#pragma once

#include <complex>
#include <span>
#include <string>
#include <vector>

#include "wspr/codec.hpp"
#include "wspr/fano.hpp"

namespace wspr {

// Samples in a slot (120 s) and where a transmission nominally starts.
inline constexpr std::size_t kSlotSamples = 45000;
inline constexpr float kStartOffset = 1.0f;

class Decoder {
public:
    struct Options {
        float min_hz = -110.0f;  // search band, relative to the centre
        float max_hz = 110.0f;
        std::size_t max_candidates = 60;
        float min_sync = 0.10f;  // normalised sync correlation to try a candidate
        FanoOptions fano;
    };

    struct Decode {
        std::string text;
        float snr_db = 0;    // in 2500 Hz, as WSJT-X gives it
        float dt_s = 0;      // start relative to 1 s into the slot
        float freq_hz = 0;   // relative to the centre (dial + 1500 Hz)
        float drift_hz = 0;  // frequency change across the transmission
    };

    Decoder() : Decoder(Options{}) {}
    explicit Decoder(const Options& options) : options_(options) {}

    // Decodes a slot: up to kSlotSamples samples from its start (fewer are
    // padded with zeros). Callsigns heard are kept for later type 3
    // messages.
    std::vector<Decode> decode(std::span<const std::complex<float>> slot);

private:
    Options options_;
    CallHashes hashes_;
};

}  // namespace wspr
