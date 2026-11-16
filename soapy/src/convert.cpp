// SPDX-License-Identifier: GPL-2.0-or-later
#include "convert.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace radioberry::soapy {
namespace {

std::int32_t get24(const std::uint8_t* p) {
    const std::uint32_t u =
        static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
    return static_cast<std::int32_t>(u << 8) >> 8;
}

void put16(std::uint8_t* p, std::int32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

// A value scaled to 16 bits; sets `clipped` when it was beyond full scale.
std::int32_t to16(double v, bool& clipped) {
    const long q = std::lround(v * kTxFullScale);
    if (q > 32767 || q < -32768) {
        clipped = true;
        return static_cast<std::int32_t>(std::clamp<long>(q, -32768, 32767));
    }
    return static_cast<std::int32_t>(q);
}

}  // namespace

std::optional<Format> parse_format(const std::string& name) {
    if (name == "CF32") {
        return Format::CF32;
    }
    if (name == "CS16") {
        return Format::CS16;
    }
    if (name == "CS32") {
        return Format::CS32;
    }
    return std::nullopt;
}

const char* format_name(Format format) {
    switch (format) {
        case Format::CF32:
            return "CF32";
        case Format::CS16:
            return "CS16";
        case Format::CS32:
            break;
    }
    return "CS32";
}

std::size_t sample_bytes(Format format) { return format == Format::CS16 ? 4 : 8; }

void rx_convert(const std::uint8_t* in, std::size_t frames, unsigned receivers, unsigned channels,
                Format format, void* const* buffs, std::size_t offset) {
    const std::size_t frame_bytes = std::size_t{6} * receivers;
    for (unsigned c = 0; c < channels; ++c) {
        const std::uint8_t* p = in + std::size_t{6} * c;
        switch (format) {
            case Format::CF32: {
                auto* out = static_cast<std::complex<float>*>(buffs[c]) + offset;
                for (std::size_t f = 0; f < frames; ++f, p += frame_bytes) {
                    out[f] = {static_cast<float>(get24(p + 3) / kRxFullScale),
                              static_cast<float>(get24(p) / kRxFullScale)};
                }
                break;
            }
            case Format::CS16: {
                auto* out = static_cast<std::int16_t*>(buffs[c]) + 2 * offset;
                for (std::size_t f = 0; f < frames; ++f, p += frame_bytes) {
                    out[2 * f] = static_cast<std::int16_t>(get24(p + 3) >> 8);
                    out[2 * f + 1] = static_cast<std::int16_t>(get24(p) >> 8);
                }
                break;
            }
            case Format::CS32: {
                auto* out = static_cast<std::int32_t*>(buffs[c]) + 2 * offset;
                for (std::size_t f = 0; f < frames; ++f, p += frame_bytes) {
                    out[2 * f] = get24(p + 3);
                    out[2 * f + 1] = get24(p);
                }
                break;
            }
        }
    }
}

std::size_t tx_convert(const void* buff, std::size_t offset, std::size_t count, Format format,
                       std::uint8_t* out) {
    std::size_t clipped_samples = 0;
    for (std::size_t k = 0; k < count; ++k, out += 4) {
        double i = 0;
        double q = 0;
        if (format == Format::CF32) {
            const std::complex<float> v = static_cast<const std::complex<float>*>(buff)[offset + k];
            i = v.real();
            q = v.imag();
        } else if (format == Format::CS16) {
            const auto* v = static_cast<const std::int16_t*>(buff) + 2 * (offset + k);
            i = v[0] / kTxFullScale;
            q = v[1] / kTxFullScale;
        } else {
            throw std::invalid_argument("CS32 is not a transmit format");
        }
        bool clipped = false;
        put16(out, to16(q, clipped));
        put16(out + 2, to16(i, clipped));
        clipped_samples += clipped ? 1 : 0;
    }
    return clipped_samples;
}

unsigned drive_code(unsigned drive) { return ((drive + 4) >> 3) + (drive >> 5); }

unsigned drive_for_code(unsigned code, unsigned ceiling) {
    unsigned best = 0;
    for (unsigned d = 0; d <= std::min(ceiling, 255u); ++d) {
        if (drive_code(d) <= code) {
            best = d;
        }
    }
    return best;
}

}  // namespace radioberry::soapy
