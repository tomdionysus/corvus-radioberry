// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/command.hpp"

#include <radioberry_ioctl.h>

#include <stdexcept>

namespace radioberry {

static_assert(Command::kRun == RADIOBERRY_CMD_RUN);
static_assert(Command::kCwx == RADIOBERRY_CMD_CWX);
static_assert(Command::kTxPermit == RADIOBERRY_CMD_TX_PERMIT);
static_assert(RADIOBERRY_CMD_PTT == 1u);

std::array<std::uint8_t, 6> Command::encode() const {
    return {flags,
            static_cast<std::uint8_t>((address & 0x3fu) << 1 | (ptt ? 1u : 0u)),
            static_cast<std::uint8_t>(data >> 24),
            static_cast<std::uint8_t>(data >> 16),
            static_cast<std::uint8_t>(data >> 8),
            static_cast<std::uint8_t>(data)};
}

Command Command::decode(std::span<const std::uint8_t, 6> b) {
    Command c;
    c.flags = b[0];
    c.address = static_cast<std::uint8_t>(b[1] >> 1);
    c.ptt = (b[1] & 1u) != 0;
    c.data = std::uint32_t{b[2]} << 24 | std::uint32_t{b[3]} << 16 | std::uint32_t{b[4]} << 8 |
             std::uint32_t{b[5]};
    return c;
}

bool Command::can_transmit() const { return ptt || (flags & (kTxPermit | kCwx)) != 0; }

Reply Reply::decode(std::span<const std::uint8_t, 6> b) {
    Reply r;
    r.external_cw_key = (b[0] & 0x04u) != 0;
    r.cw_or_ptt = (b[0] & 0x01u) != 0;
    r.transmitters = b[3] >> 6;
    r.receivers = (b[3] >> 2) & 0x0fu;
    r.fpga_type = b[3] & 0x03u;
    r.version_major = b[4];
    r.version_minor = b[5];
    return r;
}

std::optional<unsigned> rate_code_for_ksps(unsigned ksps) {
    for (unsigned code = 0; code < kRxRatesKsps.size(); ++code) {
        if (kRxRatesKsps[code] == ksps) {
            return code;
        }
    }
    return std::nullopt;
}

std::uint8_t rx_frequency_address(unsigned index) {
    if (index < 7) {
        return static_cast<std::uint8_t>(0x02 + index);
    }
    if (index < 12) {
        return static_cast<std::uint8_t>(0x12 + (index - 7));
    }
    throw std::out_of_range("receiver index " + std::to_string(index) + " above 11");
}

namespace hl2 {

Command general(unsigned rate_code, unsigned receivers) {
    if (rate_code > 3) {
        throw std::out_of_range("rate code " + std::to_string(rate_code) + " above 3");
    }
    if (receivers < 1 || receivers > 12) {
        throw std::out_of_range("receiver count " + std::to_string(receivers) + " outside 1 to 12");
    }
    Command c;
    c.address = 0x00;
    c.data = rate_code << 24 | (receivers - 1) << 3 | 1u << 2;
    return c;
}

Command rx_frequency(unsigned index, std::uint32_t hz) {
    Command c;
    c.address = rx_frequency_address(index);
    c.data = hz;
    return c;
}

Command lna_gain(int db) {
    if (db < kLnaGainMinDb || db > kLnaGainMaxDb) {
        throw std::out_of_range("LNA gain " + std::to_string(db) + " dB outside " +
                                std::to_string(kLnaGainMinDb) + " to " +
                                std::to_string(kLnaGainMaxDb));
    }
    Command c;
    c.address = 0x0a;
    // Bit 6 selects the Hermes-Lite 2 gain mode; bits 5..0 are gain + 12.
    c.data = 0x40u | static_cast<std::uint32_t>(db - kLnaGainMinDb);
    return c;
}

}  // namespace hl2

std::string version_text(unsigned major, unsigned minor) {
    return std::to_string(major) + "." + std::to_string(minor);
}

std::string fpga_text(unsigned fpga_type) {
    switch (fpga_type) {
        case RADIOBERRY_FPGA_CL016:
            return "10CL016";
        case RADIOBERRY_FPGA_CL025:
            return "10CL025";
        default:
            return "unknown (" + std::to_string(fpga_type) + ")";
    }
}

}  // namespace radioberry
