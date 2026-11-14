// SPDX-License-Identifier: GPL-2.0-or-later
//
// The .c2 files wsprd reads and wsprsim writes: a 14-byte name, a 32-bit
// period in minutes, the dial frequency in MHz as a double, then 45000
// complex samples at 375 sps as pairs of floats, all little-endian.
#pragma once

#include <complex>
#include <string>
#include <vector>

namespace wspr {

struct C2File {
    std::string name;
    double dial_mhz = 0;
    std::vector<std::complex<float>> samples;
};

// The file stores each sample conjugated (positive frequencies below the
// centre); these convert, so `samples` has them above it.

// Throws std::runtime_error when the file cannot be read or is too short.
C2File read_c2(const std::string& path);

// Writes `f`, padding or cutting the samples to one slot; `f.name` should
// be wsprd's YYMMDD_HHMM.c2. Throws std::runtime_error on failure.
void write_c2(const std::string& path, const C2File& f);

}  // namespace wspr
