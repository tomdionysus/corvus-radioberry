// SPDX-License-Identifier: GPL-2.0-or-later
//
// Sequential (Fano) decoding of WSPR's K=32 rate 1/2 convolutional code,
// written from the algorithm's description: a depth-first walk of the code
// tree that moves forward while the path metric stays above a running
// threshold and backs up to try other branches when it cannot.
#pragma once

#include <optional>
#include <span>

#include "wspr/codec.hpp"

namespace wspr {

struct FanoOptions {
    // Soft values are scaled to unit RMS; this is the assumed log-likelihood
    // slope, 2a / sigma^2, of the channel that produced them.
    float llr_scale = 2.0f;
    float delta = 1.0f;  // threshold step, in bits of metric
    long max_cycles_per_bit = 10000;
};

struct FanoResult {
    Payload payload{};
    float metric = 0;  // path metric at the end of the tree
    long cycles = 0;
};

// `soft` holds the 162 coded bits in code order (deinterleaved), positive
// for a 1. nullopt when the search gives up.
std::optional<FanoResult> fano_decode(std::span<const float, kSymbols> soft,
                                      const FanoOptions& options = {});

}  // namespace wspr
