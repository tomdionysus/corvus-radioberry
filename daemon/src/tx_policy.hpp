// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "device/control.hpp"
#include "session.hpp"

namespace radioberry::daemon {

struct TxPolicyOptions {
    // Transmit at all: without it MOX is never granted and PTT and the TX
    // permission bit stay clear in every command.
    bool allow_tx = false;
    // Let a client enable the PA (address 0x09 bit 19, which drives envpa
    // while transmitting).
    bool allow_pa = false;
    // Highest drive level sent (address 0x09 bits 31..24).
    std::uint8_t drive_ceiling = 0;
    // Longest continuous MOX; then MOX is dropped and latched until the
    // client releases it.
    std::chrono::nanoseconds watchdog = std::chrono::seconds(120);
    // The gateware has a transmitter (the driver reports one). Clear for a
    // receive-only image: MOX is then refused whatever allow_tx says.
    bool tx_available = true;
};

// What reaches the gateware's transmit controls (gateware/README.md
// sections 4, 6 and 7). MOX is granted only when transmit is allowed, a
// client owns the radio, no fault is latched and the watchdog has not
// tripped. While granted, PTT and the TX permission bit go out in every
// command; otherwise both are clear. Address 0x09 carries the client's
// drive capped at the ceiling, VNA always clear, and PA enable only while
// MOX is granted and the PA is allowed, so envpa is driven only while a
// client holds MOX. CWX is set only with MOX. Every PTT and PA change is
// logged, and so is every drive change while transmitting. This is software
// policy, not an interlock.
class TxPolicy {
public:
    TxPolicy(Control& control, Session& session, TxPolicyOptions options);

    // The client's address 0x09 word and CWX request; they take effect at
    // the next update().
    void set_tx_drive(std::uint32_t requested);
    void set_cwx(bool requested);
    // The owner's MOX request, once per packet; a client that does not
    // own the radio changes nothing.
    void update(const Owner& client, bool mox_requested, std::chrono::nanoseconds now);
    // The watchdog.
    void tick(std::chrono::nanoseconds now);

    bool transmitting() const;
    const TxPolicyOptions& options() const { return options_; }
    // The 0x09 word for the current request, transmitting or not.
    std::uint32_t effective_tx_drive(bool transmitting) const;

private:
    void apply_locked(bool granted, std::chrono::nanoseconds now);
    std::uint32_t effective_locked(bool transmitting) const;
    std::string describe_drive_locked(std::uint32_t drive) const;

    Control& control_;
    Session& session_;
    const TxPolicyOptions options_;
    mutable std::mutex mutex_;
    std::uint32_t requested_drive_ = 0;
    bool cwx_requested_ = false;
    bool granted_ = false;
    std::chrono::nanoseconds granted_since_{};
    bool pa_ = false;
    // The drive last logged while transmitting.
    std::uint32_t logged_drive_ = 0;
    bool refusal_logged_ = false;
};

}  // namespace radioberry::daemon
