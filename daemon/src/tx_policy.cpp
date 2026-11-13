// SPDX-License-Identifier: GPL-2.0-or-later
#include "tx_policy.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

namespace radioberry::daemon {
namespace {

constexpr std::uint32_t kDriveMask = 0xffu << 24;
constexpr std::uint32_t kVna = 1u << 23;
constexpr std::uint32_t kPaEnable = 1u << 19;

}  // namespace

TxPolicy::TxPolicy(Control& control, Session& session, TxPolicyOptions options)
    : control_(control), session_(session), options_(options) {}

void TxPolicy::set_tx_drive(std::uint32_t requested) {
    const std::lock_guard lock(mutex_);
    requested_drive_ = requested;
}

void TxPolicy::set_cwx(bool requested) {
    const std::lock_guard lock(mutex_);
    cwx_requested_ = requested;
}

void TxPolicy::update(const Owner& client, bool mox_requested, std::chrono::nanoseconds now) {
    const std::lock_guard lock(mutex_);
    const bool may_transmit = options_.allow_tx && options_.tx_available;
    if (mox_requested && !may_transmit) {
        if (!refusal_logged_) {
            spdlog::warn(options_.tx_available
                             ? "MOX refused: transmit is not allowed (radioberryd --allow-tx)"
                             : "MOX refused: the gateware has no transmitter (receive-only image)");
            refusal_logged_ = true;
        }
    } else if (!mox_requested) {
        refusal_logged_ = false;
    }
    session_.mox(client, mox_requested && may_transmit,
                 [&](bool granted) { apply_locked(granted, now); });
}

void TxPolicy::tick(std::chrono::nanoseconds now) {
    const std::lock_guard lock(mutex_);
    if (!granted_ || now - granted_since_ < options_.watchdog) {
        return;
    }
    granted_ = false;
    if (pa_) {
        spdlog::info("PA disabled");
        pa_ = false;
    }
    session_.fault(
        "TX watchdog: MOX held for " +
        std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(options_.watchdog).count()) +
        " s");
}

bool TxPolicy::transmitting() const {
    const std::lock_guard lock(mutex_);
    // A fault or a lost owner may have made the gateware safe since.
    return granted_ && session_.transmitting();
}

std::uint32_t TxPolicy::effective_tx_drive(bool transmitting) const {
    const std::lock_guard lock(mutex_);
    return effective_locked(transmitting);
}

std::uint32_t TxPolicy::effective_locked(bool transmitting) const {
    const std::uint32_t drive =
        std::min<std::uint32_t>(requested_drive_ >> 24, options_.drive_ceiling);
    const bool pa = (requested_drive_ & kPaEnable) != 0 && options_.allow_pa && transmitting;
    return (requested_drive_ & ~(kDriveMask | kVna | kPaEnable)) | drive << 24 |
           (pa ? kPaEnable : 0u);
}

std::string TxPolicy::describe_drive_locked(std::uint32_t drive) const {
    const std::uint32_t requested = requested_drive_ >> 24;
    if (requested > drive) {
        return fmt::format("drive {} (client asked for {}, ceiling {})", drive, requested,
                           options_.drive_ceiling);
    }
    return fmt::format("drive {} (ceiling {})", drive, options_.drive_ceiling);
}

// Under the session's lock: see Session::mox.
void TxPolicy::apply_locked(bool granted, std::chrono::nanoseconds now) {
    const std::uint32_t drive_word = effective_locked(granted);
    const std::uint32_t drive = drive_word >> 24;
    if (granted && !granted_) {
        granted_since_ = now;
        // Clients send 0x09 only every few packets, so this may still be the
        // receive value; each change while transmitting is logged below.
        spdlog::info("PTT on: {}", describe_drive_locked(drive));
        logged_drive_ = drive;
    } else if (granted && drive != logged_drive_) {
        spdlog::info("{}", describe_drive_locked(drive));
        logged_drive_ = drive;
    } else if (!granted && granted_) {
        spdlog::info("PTT off");
    }
    granted_ = granted;
    control_.set_transmit(
        {granted, granted, granted && cwx_requested_, granted && options_.allow_pa});
    if (control_.value(Control::kTxDrive) != drive_word) {
        Command c;
        c.address = Control::kTxDrive;
        c.data = drive_word;
        control_.set(c);
    }
    const bool pa = (drive_word & kPaEnable) != 0;
    if (pa != pa_) {
        spdlog::info("PA {}", pa ? "enabled (envpa follows PTT)" : "disabled");
        pa_ = pa;
    }
}

}  // namespace radioberry::daemon
