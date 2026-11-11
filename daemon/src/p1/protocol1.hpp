// SPDX-License-Identifier: GPL-2.0-or-later
//
// openHPSDR Protocol 1 wire formats in the Hermes-Lite 2 dialect, as the
// HL2 gateware's own Ethernet side produces them (usopenhpsdr1.v and
// control.v in gateware/source).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "device/rx_pump.hpp"

namespace radioberry::daemon::p1 {

inline constexpr std::uint16_t kPort = 1024;
inline constexpr std::size_t kPacketBytes = 1032;
inline constexpr std::size_t kDiscoveryReplyBytes = 60;
inline constexpr std::size_t kHalfBytes = 512;
inline constexpr std::size_t kTxSamplesPerPacket = 126;  // 63 per half
inline constexpr std::uint8_t kBoardHermesLite = 0x06;
// Receivers a Protocol 1 command can ask for: C4 bits 6..3, plus one.
inline constexpr unsigned kMaxClientReceivers = 16;

// How I/Q reaches clients. Hl2: as the Hermes-Lite 2 radio core produces
// and takes it, the first value of each pair I, which HL2 software such as
// piHPSDR expects. Standard: I and Q swapped both ways, which mirrors the
// spectrum, for clients using plain complex baseband (I real, Q
// imaginary), to which the HL2 stream is the mirror image
// (docs/examples.md).
enum class IqConvention { Hl2, Standard };

// Swaps each 6-byte receiver sample's 24-bit I and Q in place.
void swap_rx_iq(std::span<std::uint8_t> samples);
// Swaps each 4-byte TX sample's 16-bit I and Q in place.
void swap_tx_iq(std::span<std::uint8_t> samples);

enum class PacketKind { Discovery, StartStop, Data, Unknown };
// By the first four bytes: EF FE 02, EF FE 04, EF FE 01 02.
PacketKind classify(std::span<const std::uint8_t> packet);

// What discovery reports.
struct Identity {
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t version_major = 0;
    std::uint8_t version_minor = 0;
    std::uint8_t receivers = 0;
    // RX values per second the device carries (not reported; limits the
    // receivers run). 0: the Pi 5's, see device/capacity.hpp.
    std::uint32_t rx_values_max = 0;
};

// The 60-byte HL2 discovery reply: status 0x02, or 0x03 while streaming;
// board 0x06; no fixed IP, MAC change, bandscope or measurements.
std::array<std::uint8_t, kDiscoveryReplyBytes> discovery_reply(const Identity& id, bool running);

// EF FE 04 xx: bit 0 of xx starts the I/Q stream, clear stops it. Bit 1
// (wideband) is ignored: there is no wideband data.
std::optional<bool> parse_start(std::span<const std::uint8_t> packet);

// One C0..C4 block from the host.
struct HostCommand {
    std::uint8_t address = 0;  // C0 bits 6..1
    bool mox = false;          // C0 bit 0
    bool ack = false;          // C0 bit 7: the HL2 echoes the command
    std::uint32_t data = 0;    // C1..C4
};

// A 1032-byte host packet: two command blocks and 126 TX samples, each I
// then Q, 16-bit big-endian (the L/R audio bytes are dropped).
struct HostPacket {
    std::uint32_t sequence = 0;
    std::array<HostCommand, 2> commands{};
    std::array<std::uint8_t, kTxSamplesPerPacket * 4> tx_iq{};
};
std::optional<HostPacket> parse_host_packet(std::span<const std::uint8_t> packet);

// Response C0..C4. Status word `address` 0..3 (control.v): 0 carries C1
// 0x1E (TX not inhibited, no ADC overflow) and the gateware's major
// version in C4; 1 to 3 carry measurements this board does not have, as
// zero. C0 bit 0 is PTT or cw_on, bit 2 the external CW key.
using Word = std::array<std::uint8_t, 5>;
Word status_word(unsigned address, bool ptt, bool cw_key, std::uint8_t version_major);
// The echo of a command sent with C0 bit 7.
Word ack_word(std::uint8_t address, bool ptt, std::uint32_t data);

// Samples in each 512-byte half for `receivers` receivers: each sample is
// 6 bytes per receiver and 2 bytes of microphone.
constexpr std::size_t samples_per_half(unsigned receivers) {
    return (kHalfBytes - 8) / (6u * receivers + 2u);
}

// Packs device RX frames into Protocol 1 packets for the client's
// receiver count. Receivers the device does not run are sent as zeros;
// receivers it runs beyond the client's are left out.
class RxPacker {
public:
    RxPacker() { begin_packet(); }

    // Sets the client's receiver count (1 to kMaxClientReceivers); a
    // change drops the packet in progress.
    void set_receivers(unsigned receivers);
    unsigned receivers() const { return receivers_; }
    // Drops the packet in progress and restarts the sequence at 0.
    void reset();

    // Adds every frame of `block`. For each packet finished, `word` is
    // called once per half for its C0..C4, then `emit` with the packet.
    void add(const RxBlock& block, const std::function<Word()>& word,
             const std::function<void(std::span<const std::uint8_t>)>& emit);

    std::uint32_t sequence() const { return sequence_; }

    // Sends each receiver's Q before its I (IqConvention::Standard).
    void set_swap_iq(bool swap) { swap_iq_ = swap; }

private:
    void begin_packet();

    std::array<std::uint8_t, kPacketBytes> packet_{};
    unsigned receivers_ = 1;
    bool swap_iq_ = false;
    std::size_t half_ = 0;
    std::size_t sample_ = 0;
    std::uint32_t sequence_ = 0;
};

}  // namespace radioberry::daemon::p1
