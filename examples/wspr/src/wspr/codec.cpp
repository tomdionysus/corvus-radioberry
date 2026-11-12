// SPDX-License-Identifier: GPL-2.0-or-later
#include "wspr/codec.hpp"

#include <bit>
#include <cctype>
#include <cmath>
#include <numbers>
#include <sstream>

namespace wspr {

const Symbols kSync = {1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1,
                       1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0,
                       1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0,
                       1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0,
                       0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1,
                       0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0,
                       0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0};

namespace {

constexpr std::uint32_t kCallLimit = 37u * 36u * 10u * 27u * 27u * 27u;
constexpr std::uint32_t kGridLimit = 180u * 180u;

// 0-9, A-Z, space: the callsign alphabet.
int call_code(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return c == ' ' ? 36 : -1;
}

char call_char(std::uint32_t code) {
    if (code < 10) {
        return static_cast<char>('0' + code);
    }
    if (code < 36) {
        return static_cast<char>('A' + code - 10);
    }
    return ' ';
}

bool standard_power(int dbm) {
    const int nu = dbm % 10;
    return dbm >= 0 && dbm <= 60 && (nu == 0 || nu == 3 || nu == 7);
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(' ') - first + 1);
}

// Six characters: an optional letter or digit, a letter or digit, a digit,
// then up to three letters, space-padded.
std::optional<std::uint32_t> pack_call(std::string call) {
    for (char& c : call) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (call.size() >= 3 && std::isdigit(static_cast<unsigned char>(call[2]))) {
    } else if (call.size() >= 2 && std::isdigit(static_cast<unsigned char>(call[1]))) {
        call.insert(call.begin(), ' ');
    } else {
        return std::nullopt;
    }
    if (call.size() > 6) {
        return std::nullopt;
    }
    call.resize(6, ' ');
    const int c0 = call_code(call[0]);
    const int c1 = call_code(call[1]);
    const int c2 = call_code(call[2]);
    if (c0 < 0 || c1 < 0 || c1 > 35 || c2 < 0 || c2 > 9) {
        return std::nullopt;
    }
    std::uint32_t n = static_cast<std::uint32_t>(c0);
    n = n * 36 + static_cast<std::uint32_t>(c1);
    n = n * 10 + static_cast<std::uint32_t>(c2);
    for (int i = 3; i < 6; ++i) {
        const int c = call_code(call[static_cast<std::size_t>(i)]);
        if (c < 10) {
            return std::nullopt;  // a digit or not in the alphabet
        }
        n = n * 27 + static_cast<std::uint32_t>(c - 10);
    }
    return n;
}

// The six characters, untrimmed; nullopt if `n` is out of range or the
// letters after the digit have a gap.
std::optional<std::string> unpack_call(std::uint32_t n) {
    if (n >= kCallLimit) {
        return std::nullopt;
    }
    std::string call(6, ' ');
    for (int i = 5; i >= 3; --i) {
        call[static_cast<std::size_t>(i)] = call_char(n % 27 + 10);
        n /= 27;
    }
    call[2] = call_char(n % 10);
    n /= 10;
    call[1] = call_char(n % 36);
    n /= 36;
    call[0] = call_char(n);
    bool space = false;
    for (std::size_t i = 3; i < 6; ++i) {
        if (call[i] == ' ') {
            space = true;
        } else if (space) {
            return std::nullopt;
        }
    }
    return call;
}

std::optional<std::string> unpack_grid(std::uint32_t m) {
    if (m >= kGridLimit) {
        return std::nullopt;
    }
    const std::uint32_t lat = 179 - m / 180;
    const std::uint32_t lon = m % 180;
    std::string grid(4, ' ');
    grid[0] = static_cast<char>('A' + lat / 10);
    grid[1] = static_cast<char>('A' + lon / 10);
    grid[2] = static_cast<char>('0' + lat % 10);
    grid[3] = static_cast<char>('0' + lon % 10);
    return grid;
}

// Type 2: a prefix of up to three characters, or a suffix of one letter
// or one or two digits.
std::optional<std::string> add_affix(std::uint32_t n3, const std::string& call) {
    if (n3 < 60000) {
        std::string prefix(3, ' ');
        for (int i = 2; i >= 0; --i) {
            prefix[static_cast<std::size_t>(i)] = call_char(n3 % 37);
            n3 /= 37;
        }
        prefix = trim(prefix);
        if (prefix.empty()) {
            return std::nullopt;
        }
        return prefix + "/" + call;
    }
    const std::uint32_t nc = n3 - 60000;
    if (nc <= 35) {
        return call + "/" + call_char(nc);
    }
    if (nc <= 125) {
        return call + "/" + std::to_string(nc - 26);
    }
    return std::nullopt;
}

bool valid_grid6(const std::string& g) {
    return g.size() == 6 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' &&
           std::isdigit(static_cast<unsigned char>(g[2])) &&
           std::isdigit(static_cast<unsigned char>(g[3])) && g[4] >= 'A' && g[4] <= 'X' &&
           g[5] >= 'A' && g[5] <= 'X';
}

std::uint32_t rot(std::uint32_t x, int k) { return std::rotl(x, k); }

}  // namespace

std::uint8_t parity(std::uint32_t x) { return static_cast<std::uint8_t>(std::popcount(x) & 1); }

std::optional<Payload> pack(std::string_view message) {
    std::istringstream in{std::string(message)};
    std::string call, grid, extra;
    int dbm = -1;
    if (!(in >> call >> grid >> dbm) || (in >> extra) || grid.size() != 4 || !standard_power(dbm)) {
        return std::nullopt;
    }
    for (char& c : grid) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (grid[0] < 'A' || grid[0] > 'R' || grid[1] < 'A' || grid[1] > 'R' ||
        !std::isdigit(static_cast<unsigned char>(grid[2])) ||
        !std::isdigit(static_cast<unsigned char>(grid[3]))) {
        return std::nullopt;
    }
    const auto n = pack_call(call);
    if (!n) {
        return std::nullopt;
    }
    const auto g = [&](std::size_t i) { return static_cast<std::uint32_t>(grid[i]); };
    std::uint32_t m =
        (179 - 10 * (g(0) - 'A') - (g(2) - '0')) * 180 + 10 * (g(1) - 'A') + (g(3) - '0');
    m = m * 128 + static_cast<std::uint32_t>(dbm) + 64;
    // 28 bits of callsign and 22 of grid and power, most significant first.
    const std::uint64_t bits = (static_cast<std::uint64_t>(*n) << 22 | m) << 6;
    Payload p{};
    for (std::size_t i = 0; i < p.size(); ++i) {
        p[i] = static_cast<std::uint8_t>(bits >> (48 - 8 * i));
    }
    return p;
}

namespace {

// Symbol positions in the order the coded bits fill them: the 8-bit
// reversals of 0, 1, 2, ... that are below 162.
std::array<std::uint8_t, kSymbols> interleave_order() {
    std::array<std::uint8_t, kSymbols> order{};
    std::size_t p = 0;
    for (unsigned i = 0; i < 256 && p < kSymbols; ++i) {
        unsigned j = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            j |= ((i >> bit) & 1u) << (7 - bit);
        }
        if (j < kSymbols) {
            order[p++] = static_cast<std::uint8_t>(j);
        }
    }
    return order;
}

const std::array<std::uint8_t, kSymbols> kOrder = interleave_order();

}  // namespace

std::array<std::uint8_t, kSymbols> interleave(std::span<const std::uint8_t, kSymbols> coded) {
    std::array<std::uint8_t, kSymbols> out{};
    for (std::size_t p = 0; p < kSymbols; ++p) {
        out[kOrder[p]] = coded[p];
    }
    return out;
}

std::array<float, kSymbols> deinterleave(std::span<const float, kSymbols> symbols) {
    std::array<float, kSymbols> out{};
    for (std::size_t p = 0; p < kSymbols; ++p) {
        out[p] = symbols[kOrder[p]];
    }
    return out;
}

Symbols encode(const Payload& payload) {
    std::array<std::uint8_t, kSymbols> coded{};
    std::uint32_t reg = 0;
    for (int i = 0; i < kCodedBits; ++i) {
        const std::uint32_t bit =
            i < kDataBits ? (payload[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1u : 0u;
        reg = reg << 1 | bit;
        coded[static_cast<std::size_t>(2 * i)] = parity(reg & kPoly1);
        coded[static_cast<std::size_t>(2 * i + 1)] = parity(reg & kPoly2);
    }
    const auto data = interleave(coded);
    Symbols s{};
    for (std::size_t i = 0; i < kSymbols; ++i) {
        s[i] = static_cast<std::uint8_t>(kSync[i] + 2 * data[i]);
    }
    return s;
}

// Bob Jenkins' lookup3 hashlittle (public domain), byte by byte, with
// WSPR's initial value 146.
std::uint32_t call_hash(std::string_view call) {
    const auto* k = reinterpret_cast<const std::uint8_t*>(call.data());
    std::size_t length = call.size();
    std::uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + static_cast<std::uint32_t>(length) + 146u;
    const auto mix = [&] {
        a -= c, a ^= rot(c, 4), c += b;
        b -= a, b ^= rot(a, 6), a += c;
        c -= b, c ^= rot(b, 8), b += a;
        a -= c, a ^= rot(c, 16), c += b;
        b -= a, b ^= rot(a, 19), a += c;
        c -= b, c ^= rot(b, 4), b += a;
    };
    const auto word = [](const std::uint8_t* p, std::size_t n) {
        std::uint32_t w = 0;
        for (std::size_t i = 0; i < n && i < 4; ++i) {
            w += static_cast<std::uint32_t>(p[i]) << (8 * i);
        }
        return w;
    };
    while (length > 12) {
        a += word(k, 4);
        b += word(k + 4, 4);
        c += word(k + 8, 4);
        mix();
        length -= 12;
        k += 12;
    }
    if (length == 0) {
        return c & 32767u;
    }
    a += word(k, length);
    if (length > 4) {
        b += word(k + 4, length - 4);
    }
    if (length > 8) {
        c += word(k + 8, length - 8);
    }
    c ^= b, c -= rot(b, 14);
    a ^= c, a -= rot(c, 11);
    b ^= a, b -= rot(a, 25);
    c ^= b, c -= rot(b, 16);
    a ^= c, a -= rot(c, 4);
    b ^= a, b -= rot(a, 14);
    c ^= b, c -= rot(b, 24);
    return c & 32767u;
}

std::optional<std::string> CallHashes::find(std::uint32_t hash) const {
    const auto it = calls_.find(hash);
    if (it == calls_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> unpack(const Payload& payload, CallHashes& hashes) {
    std::uint64_t bits = 0;
    for (const std::uint8_t byte : payload) {
        bits = bits << 8 | byte;
    }
    bits >>= 6;
    const auto n1 = static_cast<std::uint32_t>(bits >> 22);
    const auto n2 = static_cast<std::uint32_t>(bits & 0x3fffff);
    const auto raw = unpack_call(n1);
    if (!raw) {
        return std::nullopt;
    }
    const std::string call = trim(*raw);
    const int ntype = static_cast<int>(n2 & 127) - 64;
    const std::uint32_t m = n2 >> 7;
    if (ntype >= 0 && ntype <= 62) {
        const int nu = ntype % 10;
        if (nu == 0 || nu == 3 || nu == 7) {
            const auto grid = unpack_grid(m);
            if (!grid) {
                return std::nullopt;
            }
            hashes.add(call);
            return call + " " + *grid + " " + std::to_string(ntype);
        }
        const int nadd = nu > 7 ? nu - 7 : nu > 3 ? nu - 3 : nu;
        const auto full = add_affix(m + 32768u * static_cast<std::uint32_t>(nadd - 1), call);
        if (!full) {
            return std::nullopt;
        }
        hashes.add(*full);
        return *full + " " + std::to_string(ntype - nadd);
    }
    if (ntype < 0) {
        const int dbm = -(ntype + 1);
        if (!standard_power(dbm)) {
            return std::nullopt;
        }
        // The locator is sent rotated one place left, as "K52UDF" for FK52UD.
        const std::string grid6 = (*raw)[5] + raw->substr(0, 5);
        if (!valid_grid6(grid6)) {
            return std::nullopt;
        }
        const auto heard = hashes.find(m);
        return "<" + heard.value_or("...") + "> " + grid6 + " " + std::to_string(dbm);
    }
    return std::nullopt;
}

std::vector<std::complex<float>> synthesize(const Symbols& symbols, float freq_hz, float rate) {
    const auto per_symbol =
        static_cast<std::size_t>(std::lround(rate * kSymbolSamples / kSampleRate));
    std::vector<std::complex<float>> out;
    out.reserve(per_symbol * kSymbols);
    double phase = 0;
    for (const std::uint8_t s : symbols) {
        const double step =
            2 * std::numbers::pi * (freq_hz + (static_cast<double>(s) - 1.5) * kToneSpacing) / rate;
        for (std::size_t i = 0; i < per_symbol; ++i) {
            out.emplace_back(static_cast<float>(std::cos(phase)),
                             static_cast<float>(std::sin(phase)));
            phase = std::fmod(phase + step, 2 * std::numbers::pi);
        }
    }
    return out;
}

}  // namespace wspr
