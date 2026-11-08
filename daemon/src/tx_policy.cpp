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

void TxPolicy::update(bool mox_requested, std::chrono::nanoseconds now) {
    const std::lock_guard lock(mutex_);
    if (mox_requested && !options_.allow_tx) {
        if (!refusal_logged_) {
            spdlog::warn("MOX refused: transmit is not allowed (radioberryd --allow-tx)");
            refusal_logged_ = true;
        }
    } else if (!mox_requested) {
        refusal_logged_ = false;
    }
    session_.mox(mox_requested && options_.allow_tx,
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
    return granted_;
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

// Under the session's lock: see Session::mox.
void TxPolicy::apply_locked(bool granted, std::chrono::nanoseconds now) {
    const std::uint32_t drive_word = effective_locked(granted);
    if (granted && !granted_) {
        granted_since_ = now;
        spdlog::info("PTT on: drive {} (ceiling {})", drive_word >> 24, options_.drive_ceiling);
    } else if (!granted && granted_) {
        spdlog::info("PTT off");
    }
    granted_ = granted;
    control_.set_transmit({granted, granted, granted && cwx_requested_});
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
