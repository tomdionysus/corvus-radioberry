// SPDX-License-Identifier: GPL-2.0-or-later
#include "session.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace radioberry::daemon {

std::string describe(const Peer& p) {
    return std::to_string(p.address >> 24) + "." + std::to_string((p.address >> 16) & 0xffu) + "." +
           std::to_string((p.address >> 8) & 0xffu) + "." + std::to_string(p.address & 0xffu) +
           ":" + std::to_string(p.port) + (p.tcp ? " (TCP)" : " (UDP)");
}

std::string describe(const Owner& o) {
    return std::string(o.protocol == Protocol::P1 ? "Protocol 1" : "Protocol 2") + " client " +
           describe(o.peer);
}

bool is_p2_discovery(std::span<const std::uint8_t> p) {
    return p.size() == 60 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0 && p[4] == 0x02;
}

Session::Session(MakeSafe make_safe, std::chrono::nanoseconds timeout)
    : make_safe_(std::move(make_safe)), timeout_(timeout) {}

bool Session::claim(const Owner& client, std::chrono::nanoseconds now) {
    const std::lock_guard lock(mutex_);
    if (owner_ && !(*owner_ == client)) {
        return false;
    }
    if (!owner_) {
        owner_ = client;
        mox_ = false;
        latched_ = false;
        spdlog::info("{} owns the radio", describe(client));
    }
    last_heard_ = now;
    return true;
}

bool Session::owns(const Owner& client) const {
    const std::lock_guard lock(mutex_);
    return owner_ && *owner_ == client;
}

bool Session::busy_for(const Owner& client) const {
    const std::lock_guard lock(mutex_);
    return owner_ && !(*owner_ == client);
}

std::optional<Owner> Session::owner() const {
    const std::lock_guard lock(mutex_);
    return owner_;
}

void Session::heard(const Owner& client, std::chrono::nanoseconds now) {
    const std::lock_guard lock(mutex_);
    if (owner_ && *owner_ == client) {
        last_heard_ = now;
    }
}

void Session::release(const Owner& client, const std::string& why) {
    const std::lock_guard lock(mutex_);
    if (owner_ && *owner_ == client) {
        drop_locked(why);
    }
}

std::optional<Owner> Session::tick(std::chrono::nanoseconds now) {
    const std::lock_guard lock(mutex_);
    if (!owner_ || now - last_heard_ < timeout_) {
        return std::nullopt;
    }
    const Owner gone = *owner_;
    drop_locked(
        "silent for " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(timeout_).count()) +
        " ms");
    return gone;
}

void Session::fault(const std::string& why) {
    const std::lock_guard lock(mutex_);
    ++faults_;
    if (!mox_) {
        spdlog::debug("stream fault without MOX: {}", why);
        return;
    }
    mox_ = false;
    latched_ = true;
    make_safe_();
    spdlog::warn("stream fault while transmitting: {}; MOX dropped until the client releases it",
                 why);
}

bool Session::mox(bool requested, const std::function<void(bool)>& apply) {
    const std::lock_guard lock(mutex_);
    if (!requested) {
        if (latched_) {
            spdlog::info("MOX released; fault latch cleared");
        }
        latched_ = false;
    }
    mox_ = requested && !latched_ && owner_.has_value();
    if (apply) {
        apply(mox_);
    }
    return mox_;
}

bool Session::transmitting() const {
    const std::lock_guard lock(mutex_);
    return mox_;
}

bool Session::latched() const {
    const std::lock_guard lock(mutex_);
    return latched_;
}

std::uint64_t Session::faults() const {
    const std::lock_guard lock(mutex_);
    return faults_;
}

void Session::drop_locked(const std::string& why) {
    spdlog::info("{} released the radio: {}", describe(*owner_), why);
    owner_.reset();
    mox_ = false;
    latched_ = false;
    make_safe_();
}

}  // namespace radioberry::daemon
