// SPDX-License-Identifier: GPL-2.0-or-later
#include "wspr/c2file.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "wspr/decoder.hpp"

namespace wspr {

C2File read_c2(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    constexpr std::size_t kHeader = 14 + 4 + 8;
    if (bytes.size() < kHeader + kSlotSamples * 8) {
        throw std::runtime_error(path + " is not a .c2 file of " + std::to_string(kSlotSamples) +
                                 " samples");
    }
    C2File f;
    f.name.assign(bytes.data(), 14);
    f.name.erase(f.name.find_last_not_of(std::string("\0 ", 2)) + 1);
    std::memcpy(&f.dial_mhz, bytes.data() + 18, sizeof f.dial_mhz);
    f.samples.resize(kSlotSamples);
    for (std::size_t i = 0; i < kSlotSamples; ++i) {
        float iq[2];
        std::memcpy(iq, bytes.data() + kHeader + 8 * i, sizeof iq);
        f.samples[i] = {iq[0], -iq[1]};
    }
    return f;
}

void write_c2(const std::string& path, const C2File& f) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot create " + path);
    }
    // A fixed 14-byte field, zero-padded, not a C string.
    std::array<char, 14> name{};
    std::copy_n(f.name.begin(), std::min(f.name.size(), name.size()), name.begin());
    const std::int32_t minutes = 2;
    out.write(name.data(), static_cast<std::streamsize>(name.size()));
    out.write(reinterpret_cast<const char*>(&minutes), sizeof minutes);
    out.write(reinterpret_cast<const char*>(&f.dial_mhz), sizeof f.dial_mhz);
    for (std::size_t i = 0; i < kSlotSamples; ++i) {
        const std::complex<float> s = i < f.samples.size() ? f.samples[i] : std::complex<float>{};
        const float iq[2] = {s.real(), -s.imag()};
        out.write(reinterpret_cast<const char*>(iq), sizeof iq);
    }
    if (!out) {
        throw std::runtime_error("cannot write " + path);
    }
}

}  // namespace wspr
