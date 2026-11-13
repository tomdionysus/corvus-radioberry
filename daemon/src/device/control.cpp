// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/control.hpp"

#include <stdexcept>
#include <string>

namespace radioberry::daemon {

void Control::set(const Command& command) {
    if (command.address >= kAddresses) {
        throw std::invalid_argument("command address " + std::to_string(command.address) +
                                    " above 0x3f");
    }
    if (command.ptt || (command.flags & ~Command::kRun) != 0) {
        throw std::invalid_argument("command carries transmit bits; use set_transmit");
    }
    if (disables_tx_watchdog(command)) {
        throw std::invalid_argument("PTT hang time 31 disables the gateware's TX watchdog");
    }
    {
        const std::lock_guard lock(mutex_);
        values_[command.address] = command.data;
        queue_locked(command.address, false);
    }
    notify();
}

void Control::set_transmit(TransmitState state) {
    {
        const std::lock_guard lock(mutex_);
        if (state == transmit_) {
            return;
        }
        transmit_ = state;
        bits_changed_ = true;
    }
    notify();
}

void Control::set_run(bool run) {
    {
        const std::lock_guard lock(mutex_);
        if (run == run_) {
            return;
        }
        run_ = run;
        bits_changed_ = true;
    }
    notify();
}

void Control::safe() {
    {
        const std::lock_guard lock(mutex_);
        if (transmit_ != TransmitState{}) {
            transmit_ = {};
            bits_changed_ = true;
        }
        values_[kTxDrive] = values_[kTxDrive].value_or(0) & ~kTxDriveHazards;
        queue_locked(kTxDrive, true);
    }
    notify();
}

std::optional<Command> Control::next() {
    const std::lock_guard lock(mutex_);
    if (!queue_.empty()) {
        const std::uint8_t address = queue_.front();
        queue_.pop_front();
        queued_[address] = false;
        return stamp_locked(address);
    }
    if (bits_changed_ && last_sent_) {
        return stamp_locked(*last_sent_);
    }
    return std::nullopt;
}

TransmitState Control::transmit() const {
    const std::lock_guard lock(mutex_);
    return transmit_;
}

bool Control::run() const {
    const std::lock_guard lock(mutex_);
    return run_;
}

std::optional<std::uint32_t> Control::value(std::uint8_t address) const {
    const std::lock_guard lock(mutex_);
    return address < kAddresses ? values_[address] : std::nullopt;
}

std::size_t Control::pending() const {
    const std::lock_guard lock(mutex_);
    return queue_.size() + (queue_.empty() && bits_changed_ && last_sent_ ? 1 : 0);
}

void Control::queue_locked(std::uint8_t address, bool front) {
    if (queued_[address]) {
        if (!front || queue_.front() == address) {
            return;
        }
        std::erase(queue_, address);
    }
    queued_[address] = true;
    if (front) {
        queue_.push_front(address);
    } else {
        queue_.push_back(address);
    }
}

Command Control::stamp_locked(std::uint8_t address) {
    Command c;
    c.flags = static_cast<std::uint8_t>(
        (run_ ? Command::kRun : 0) | (transmit_.tx_permit ? Command::kTxPermit : 0) |
        (transmit_.cwx ? Command::kCwx : 0) | (transmit_.pa_allowed ? Command::kPaAllowed : 0));
    c.address = address;
    c.ptt = transmit_.ptt;
    c.data = values_[address].value_or(0);
    bits_changed_ = false;
    last_sent_ = address;
    return c;
}

}  // namespace radioberry::daemon
