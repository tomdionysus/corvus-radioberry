// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "radioberry/command.hpp"

namespace radioberry::daemon {

// The transmit bits every outgoing command carries. All clear is receive
// only.
struct TransmitState {
    bool ptt = false;        // C0 bit 0, SPI bit 32
    bool tx_permit = false;  // SPI bit 42: gates the CN301 T/R and PA lines
    bool cwx = false;        // SPI bit 41: CW keying from the TX I/Q LSBs
    bool operator==(const TransmitState&) const = default;
};

// The gateware's command registers as the daemon wants them, and the
// queue of those still to be sent. Protocol code sets registers from any
// thread; the device thread takes commands with next(). PTT, run and the
// TX permission bit travel in every command (gateware/README.md section
// 4), so they are held here and stamped onto each command rather than
// taken from it.
class Control {
public:
    static constexpr std::uint8_t kAddresses = 64;
    static constexpr std::uint8_t kGeneral = 0x00;
    // Drive (bits 31..24), VNA (23), PA enable (19), T/R relay disable (18).
    static constexpr std::uint8_t kTxDrive = 0x09;
    // Bits of kTxDrive that safe() clears: drive, VNA and PA enable.
    static constexpr std::uint32_t kTxDriveHazards = 0xff000000u | 1u << 23 | 1u << 19;

    // Sets register `command.address` to `command.data` and queues it; the
    // latest value per address is what is sent. Throws
    // std::invalid_argument for an address above 0x3f, for PTT or any
    // byte-0 flag other than run (set_transmit() and set_run() own those),
    // and for a command that disables the gateware's TX watchdog.
    void set(const Command& command);
    // Sets the transmit bits for every command from now on. A change goes
    // out with the next command; with none queued, the last register sent
    // is sent again to carry it.
    void set_transmit(TransmitState state);
    void set_run(bool run);
    // Fail-safe: clears the transmit bits and queues address 0x09 with
    // drive, VNA and PA enable cleared ahead of every other command.
    void safe();

    // Device thread: the next command to send, stamped with run and the
    // transmit bits, or nothing.
    std::optional<Command> next();

    TransmitState transmit() const;
    bool run() const;
    // The value last set for `address`, sent or not.
    std::optional<std::uint32_t> value(std::uint8_t address) const;
    // Commands waiting for next(), counting a pending carrier.
    std::size_t pending() const;

private:
    void queue_locked(std::uint8_t address, bool front);
    Command stamp_locked(std::uint8_t address);

    mutable std::mutex mutex_;
    std::array<std::optional<std::uint32_t>, kAddresses> values_{};
    std::array<bool, kAddresses> queued_{};
    std::deque<std::uint8_t> queue_;
    TransmitState transmit_;
    // Clear until a client streams: the driver moves RX only while it is
    // set (radioberry_ioctl.h).
    bool run_ = false;
    // The transmit bits or run changed since the last command went out.
    bool bits_changed_ = false;
    std::optional<std::uint8_t> last_sent_;
};

}  // namespace radioberry::daemon
