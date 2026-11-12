// SPDX-License-Identifier: GPL-2.0-or-later
//
// The WSPR message and channel code: 50-bit messages (types 1, 2 and 3),
// the K=32 rate 1/2 convolutional code, the bit-reversal interleaver and
// the 162 four-tone channel symbols, each tone's low bit the sync vector.
// Written from the published WSPR protocol description.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wspr {

inline constexpr int kSymbols = 162;
inline constexpr int kDataBits = 50;
inline constexpr int kCodedBits = 81;  // the 50 data bits and 31 zero tail bits
inline constexpr float kSampleRate = 375.0f;
inline constexpr int kSymbolSamples = 256;                           // at 375 sps: 0.683 s
inline constexpr float kToneSpacing = kSampleRate / kSymbolSamples;  // 1.4648 Hz

// The 50 message bits, most significant first; the last 6 bits are zero.
using Payload = std::array<std::uint8_t, 7>;
using Symbols = std::array<std::uint8_t, kSymbols>;

extern const Symbols kSync;

// Packs a type 1 message, "CALL GRID4 DBM"; nullopt if it is not one.
std::optional<Payload> pack(std::string_view message);

// The channel symbols, 0 to 3, for a payload.
Symbols encode(const Payload& payload);

// The 15-bit hash a type 3 message carries for a callsign.
std::uint32_t call_hash(std::string_view call);

// Callsigns heard in type 1 and 2 messages, by hash, for type 3 messages.
class CallHashes {
public:
    void add(const std::string& call) { calls_[call_hash(call)] = call; }
    std::optional<std::string> find(std::uint32_t hash) const;

private:
    std::unordered_map<std::uint32_t, std::string> calls_;
};

// The message text, as WSJT-X prints it ("K1ABC FN42 33", "PJ4/K1ABC 33",
// "<PJ4/K1ABC> FK52UD 33"; "<...>" when the hash is unknown), or nullopt
// when the bits are not a valid message. Callsigns in type 1 and 2
// messages are added to `hashes`.
std::optional<std::string> unpack(const Payload& payload, CallHashes& hashes);

// Bit-reversal interleaving of the 162 coded bits into symbol order, and
// its inverse for soft values.
std::array<std::uint8_t, kSymbols> interleave(std::span<const std::uint8_t, kSymbols> coded);
std::array<float, kSymbols> deinterleave(std::span<const float, kSymbols> symbols);

// The convolutional code's two output bits after shifting in one bit.
std::uint8_t parity(std::uint32_t x);
inline constexpr std::uint32_t kPoly1 = 0xf2d05351;
inline constexpr std::uint32_t kPoly2 = 0xe4613c47;

// Continuous-phase four-tone signal for `symbols`, centred on `freq_hz`
// (tone 0 is 1.5 spacings below it), unit amplitude, at `rate`.
std::vector<std::complex<float>> synthesize(const Symbols& symbols, float freq_hz, float rate);

}  // namespace wspr
