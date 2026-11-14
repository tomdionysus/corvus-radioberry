// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace radioberry {

// One gateware command, the radioberry_command layout of the driver's ABI:
// byte 0 carries run, CWX, the TX permission bit and PA allowed, bytes 1
// to 5 a Hermes-Lite 2 C0..C4 block. PTT and the TX permission bit are
// clear unless set explicitly.
struct Command {
    static constexpr std::uint8_t kRun = 1u << 0;
    static constexpr std::uint8_t kCwx = 1u << 1;
    static constexpr std::uint8_t kTxPermit = 1u << 2;
    static constexpr std::uint8_t kPaAllowed = 1u << 3;

    std::uint8_t flags = kRun;  // byte 0
    std::uint8_t address = 0;   // C0 bits 7..1, 0 to 0x3f
    bool ptt = false;           // C0 bit 0
    std::uint32_t data = 0;     // C1..C4, C1 most significant

    std::array<std::uint8_t, 6> encode() const;
    static Command decode(std::span<const std::uint8_t, 6> bytes);

    // True if this command can key the transmitter or arm its outputs:
    // PTT, the TX permission bit or CWX.
    bool can_transmit() const;

    bool operator==(const Command&) const = default;
};

// The gateware's reply to every command.
struct Reply {
    bool external_cw_key = false;  // byte 0 bit 2
    bool cw_or_ptt = false;        // byte 0 bit 0: cw_on or external PTT
    unsigned transmitters = 0;
    unsigned receivers = 0;
    unsigned fpga_type = 0;  // 1 = CL016, 2 = CL025
    unsigned version_major = 0;
    unsigned version_minor = 0;

    static Reply decode(std::span<const std::uint8_t, 6> bytes);
};

// Why a command could key the transmitter or arm the PA: "PTT", "TX
// permission", "CWX", and for address 0x09 "drive", "VNA" and "PA
// enable". Empty for a receive-only command.
std::vector<std::string> transmit_hazards(const Command& command);

// True for an address 0x17 command with PTT hang time 31, which disables
// the Hermes-Lite 2 core's transmit watchdog. Never to be sent.
bool disables_tx_watchdog(const Command& command);

// Receive sample rates the gateware offers, by Hermes-Lite 2 rate code.
inline constexpr std::array<unsigned, 4> kRxRatesKsps = {48, 96, 192, 384};
// Rate code (0 to 3) for a rate in ksps, or nothing.
std::optional<unsigned> rate_code_for_ksps(unsigned ksps);

// Address of receiver `index` (0-based) frequency: 0x02..0x08, then
// 0x12..0x16, as in the Hermes-Lite 2 command set.
std::uint8_t rx_frequency_address(unsigned index);

// LNA gain range in dB.
inline constexpr int kLnaGainMinDb = -12;
inline constexpr int kLnaGainMaxDb = 48;

// Hermes-Lite 2 commands. None of them sets PTT or the TX permission bit.
namespace hl2 {

// Address 0x00: rate code, receivers (1 to 12) and duplex. Duplex is set,
// so receiver 1 follows its own frequency rather than the TX frequency.
Command general(unsigned rate_code, unsigned receivers);
// Receiver `index` (0-based) frequency in Hz.
Command rx_frequency(unsigned index, std::uint32_t hz);
// Address 0x0a: LNA gain in dB, kLnaGainMinDb to kLnaGainMaxDb.
Command lna_gain(int db);

}  // namespace hl2

// "75.3"
std::string version_text(unsigned major, unsigned minor);
// "1.2.3" from radioberry_info.driver_version.
std::string driver_version_text(std::uint32_t version);
// "10CL016", "10CL025" or "unknown (N)" for a RADIOBERRY_FPGA_* value.
std::string fpga_text(unsigned fpga_type);

}  // namespace radioberry
