// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace radioberry::soapy {

// Stream sample formats, by their SoapySDR names.
enum class Format { CF32, CS16, CS32 };

// The format named `name` ("CF32", "CS16", "CS32"), or nothing.
std::optional<Format> parse_format(const std::string& name);
const char* format_name(Format format);
// Bytes of one complex sample in `format`.
std::size_t sample_bytes(Format format);

// Full scale of the device's 24-bit receive values and 16-bit transmit
// values.
inline constexpr double kRxFullScale = 8388608.0;  // 2^23
inline constexpr double kTxFullScale = 32768.0;    // 2^15

// Receive: `frames` frames of the driver's layout (per frame, for each of
// `receivers` receivers, a 24-bit big-endian value then another) into the
// first `channels` buffers, starting at element `offset` of each. The two
// values of each sample are swapped, as radioberryd does with
// `iq_convention: standard`: in the device's (Hermes-Lite 2) order a plain
// complex-baseband client sees the spectrum mirrored. CF32 is scaled to 1.0
// full scale, CS16 keeps the top 16 bits, CS32 holds the 24-bit value.
void rx_convert(const std::uint8_t* in, std::size_t frames, unsigned receivers, unsigned channels,
                Format format, void* const* buffs, std::size_t offset);

// Transmit: `count` samples of buffer `buff`, from element `offset`, into
// the driver's layout (two 16-bit big-endian values, swapped as on
// receive). Values beyond full scale are clipped. Returns how many samples
// were clipped. CS32 is not a transmit format.
std::size_t tx_convert(const void* buff, std::size_t offset, std::size_t count, Format format,
                       std::uint8_t* out);

// The radio's TX gain codes, as the gateware derives them from the drive
// byte (gateware/rtl/rpi5/rbv_rpi5_top.sv): 0 to 39, 0.5 dB per code at the
// AD9866 by its datasheet.
unsigned drive_code(unsigned drive);
// The highest drive byte from 0 to `ceiling` whose code is at most `code`
// (0 when none is).
unsigned drive_for_code(unsigned code, unsigned ceiling);

}  // namespace radioberry::soapy
