// SPDX-License-Identifier: GPL-2.0-or-later
//
// openHPSDR Protocol 2 wire formats, as the reference client and radio
// simulator in piHPSDR (new_protocol.c, newhpsdrsim.c) send and read them.
// Every multi-byte field is big-endian; every packet starts with a 32-bit
// sequence number.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace radioberry::daemon::p2 {

// UDP ports as offsets from the base port (1024). Host to radio: General
// (and discovery) on 0, Receiver Specific 1, Transmitter Specific 2, High
// Priority 3, audio 4, TX I/Q 5. Radio to host, by source port: command
// response 0, High Priority status 1, Mic/Line 2, wideband 3, DDC n I/Q
// 11 + n.
inline constexpr unsigned kGeneralPort = 0;
inline constexpr unsigned kRxSpecificPort = 1;
inline constexpr unsigned kTxSpecificPort = 2;
inline constexpr unsigned kHighPriorityPort = 3;
inline constexpr unsigned kAudioPort = 4;
inline constexpr unsigned kTxIqPort = 5;
inline constexpr unsigned kHighPriorityStatusPort = 1;
inline constexpr unsigned kMicLinePort = 2;
inline constexpr unsigned kDdcPort0 = 11;
// DDCs a Receiver Specific packet can address.
inline constexpr unsigned kMaxDdcs = 80;

inline constexpr std::size_t kShortPacketBytes = 60;
inline constexpr std::size_t kLongPacketBytes = 1444;
inline constexpr std::size_t kMicPacketBytes = 132;
inline constexpr std::size_t kDdcSamplesPerPacket = 238;
inline constexpr std::size_t kDdcHeaderBytes = 16;
inline constexpr std::size_t kTxSamplesPerPacket = 240;
inline constexpr std::size_t kMicSamplesPerPacket = 64;
inline constexpr std::size_t kSampleBytes = 6;  // 24-bit I then Q
inline constexpr std::uint32_t kTxRateHz = 192000;
inline constexpr std::uint32_t kMicRateHz = 48000;
// Board 6 is the Hermes-Lite; a software version of 40 or more makes
// piHPSDR treat it as a Hermes-Lite 2.
inline constexpr std::uint8_t kBoardHermesLite = 0x06;
// The Protocol 2 version reported, as the piHPSDR simulator does (3.8).
inline constexpr std::uint8_t kProtocolVersion = 38;

using ShortPacket = std::array<std::uint8_t, kShortPacketBytes>;
using LongPacket = std::array<std::uint8_t, kLongPacketBytes>;
using MicPacket = std::array<std::uint8_t, kMicPacketBytes>;

std::uint32_t sequence_of(std::span<const std::uint8_t> packet);

// What discovery reports.
struct Identity {
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t version_major = 0;  // the gateware's
    std::uint8_t receivers = 0;      // DDCs
};

// The reply to discovery (60 bytes, 00 00 00 00 02; session.hpp,
// is_p2_discovery): status 2, or 3 while a client streams; MAC; board;
// protocol and software versions; DDC count.
ShortPacket discovery_reply(const Identity& id, bool running);

// A General packet: 60 bytes with byte 4 zero, after any sequence number.
// Byte 4 nonzero marks the discovery, erase and set-IP commands instead.
bool is_general(std::span<const std::uint8_t> packet);

struct General {
    std::uint32_t sequence = 0;
    // Bytes 5 to 22: the client's port choices; zero is the default.
    // Anything else is not supported and reported.
    bool default_ports = true;
    bool wideband = false;     // byte 23
    bool phase_words = false;  // byte 37 bit 3: frequencies as phase words
    bool pa_enable = false;    // byte 58 bit 0
};
std::optional<General> parse_general(std::span<const std::uint8_t> packet);

struct Ddc {
    bool enabled = false;
    std::uint8_t adc = 0;
    std::uint16_t rate_ksps = 0;
    std::uint8_t bits = 0;
    std::uint8_t sync = 0;  // DDCs this one is synchronised with
};

// 1444 bytes on port 1: ADC count (byte 4), DDC enable bits from byte 7,
// and per DDC n the ADC (17 + 6n), rate in ksps (18 + 6n), sample bits
// (22 + 6n) and synchronisation map (1363 + n).
struct RxSpecific {
    std::uint32_t sequence = 0;
    std::uint8_t adcs = 0;
    std::array<Ddc, kMaxDdcs> ddc{};
};
std::optional<RxSpecific> parse_rx_specific(std::span<const std::uint8_t> packet);

// 60 bytes on port 2: DAC count (4), CW mode bits (5), DUC rate in ksps
// (14..15) and sample bits (16). Zero rate and bits mean 192 ksps and 24.
struct TxSpecific {
    std::uint32_t sequence = 0;
    std::uint8_t dacs = 0;
    std::uint8_t cw_mode = 0;
    std::uint16_t rate_ksps = 0;
    std::uint8_t bits = 0;
};
std::optional<TxSpecific> parse_tx_specific(std::span<const std::uint8_t> packet);

// DDC frequencies a High Priority packet carries (bytes 9 + 4n).
inline constexpr unsigned kHighPriorityDdcs = 80;

// 1444 bytes on port 3: run and MOX (byte 4 bits 0 and 1), CWX (byte 5
// bit 0), DDC frequencies (9 + 4n), DUC frequency (329), drive (345),
// open-collector outputs (1401), step attenuators of ADC1 (1442) and ADC0
// (1443). Frequencies are as sent: Hz, or phase words (General).
struct HighPriority {
    std::uint32_t sequence = 0;
    bool run = false;
    bool mox = false;
    bool cwx = false;
    std::array<std::uint32_t, kHighPriorityDdcs> ddc_frequency{};
    std::uint32_t duc_frequency = 0;
    std::uint8_t drive = 0;
    std::uint8_t open_collector = 0;
    std::uint8_t attenuation_adc0 = 0;
    std::uint8_t attenuation_adc1 = 0;
};
std::optional<HighPriority> parse_high_priority(std::span<const std::uint8_t> packet);

// Hz for a phase word against the protocol's 122.88 MHz clock.
std::uint32_t frequency_from_phase_word(std::uint32_t phase);

// 1444 bytes on port 5: 240 samples at 192 ksps, I then Q, 24-bit.
struct TxIq {
    std::uint32_t sequence = 0;
    std::span<const std::uint8_t> samples;  // into the packet
};
std::optional<TxIq> parse_tx_iq(std::span<const std::uint8_t> packet);

// A DDC I/Q packet: sequence, an 8-byte zero timestamp, 24 bits per sample,
// 238 samples.
void begin_ddc_packet(LongPacket& packet, std::uint32_t sequence);

// What the radio reports at High Priority: bit 0 of byte 4 the PTT input,
// bit 1 the dot (the HL2's CW key). No power, voltage or ADC measurements.
ShortPacket high_priority_status(std::uint32_t sequence, bool ptt, bool cw_key);

// 64 microphone samples of silence: the radio has no microphone, but the
// packets pace the client's transmitter.
MicPacket mic_packet(std::uint32_t sequence);

}  // namespace radioberry::daemon::p2
