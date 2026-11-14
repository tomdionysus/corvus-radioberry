// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <system_error>

namespace radioberry::daemon {

// An eventfd: a thread sleeps in poll() on fd() until another signals it,
// so no thread wakes on a timer to look for work. signal() is
// async-signal-safe. Throws std::system_error if the eventfd cannot be
// made.
class Event {
public:
    Event() : fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "eventfd");
        }
    }
    ~Event() { ::close(fd_); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    int fd() const { return fd_; }
    void signal() const noexcept {
        const std::uint64_t one = 1;
        [[maybe_unused]] const ssize_t n = ::write(fd_, &one, sizeof one);
    }
    // Clears the signal after poll() saw it.
    void drain() const noexcept {
        std::uint64_t count = 0;
        [[maybe_unused]] const ssize_t n = ::read(fd_, &count, sizeof count);
    }

private:
    int fd_;
};

}  // namespace radioberry::daemon
