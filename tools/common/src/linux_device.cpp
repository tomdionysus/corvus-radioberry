// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Device interface over the radioberry.ko character device. Hardware
// glue, exercised on the Pi rather than by the unit tests.
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>

#include "radioberry/device.hpp"

namespace radioberry {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

class LinuxDevice final : public Device {
public:
    LinuxDevice(const std::string& path, bool nonblocking)
        : path_(path),
          fd_(::open(path.c_str(), O_RDWR | O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0))) {
        if (fd_ < 0) {
            fail(path);
        }
    }
    ~LinuxDevice() override { ::close(fd_); }
    LinuxDevice(const LinuxDevice&) = delete;
    LinuxDevice& operator=(const LinuxDevice&) = delete;

    radioberry_info info() override {
        radioberry_info info{};
        if (::ioctl(fd_, RADIOBERRY_IOC_INFO, &info) != 0) {
            fail(path_ + ": info");
        }
        return info;
    }

    radioberry_stats stats() override {
        radioberry_stats stats{};
        if (::ioctl(fd_, RADIOBERRY_IOC_STATS, &stats) != 0) {
            fail(path_ + ": statistics");
        }
        return stats;
    }

    Reply command(const Command& command) override {
        radioberry_command c{};
        const auto bytes = command.encode();
        std::memcpy(c.tx, bytes.data(), bytes.size());
        if (::ioctl(fd_, RADIOBERRY_IOC_COMMAND, &c) != 0) {
            fail(path_ + ": command");
        }
        return Reply::decode(std::span<const std::uint8_t, 6>(c.rx, 6));
    }

    std::size_t read(std::span<std::uint8_t> buffer) override {
        const ssize_t n = ::read(fd_, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return 0;
            }
            fail(path_ + ": read");
        }
        return static_cast<std::size_t>(n);
    }

    std::size_t write(std::span<const std::uint8_t> buffer) override {
        const ssize_t n = ::write(fd_, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return 0;
            }
            fail(path_ + ": write");
        }
        return static_cast<std::size_t>(n);
    }

    Readiness wait(bool want_read, bool want_write, int timeout_ms) override {
        pollfd p{fd_, static_cast<short>((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0)), 0};
        const int n = ::poll(&p, 1, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                return {};
            }
            fail(path_ + ": poll");
        }
        return {(p.revents & POLLIN) != 0, (p.revents & POLLOUT) != 0,
                (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0};
    }

    Readiness wait_or_wake(bool want_read, bool want_write, int timeout_ms, int wake_fd) override {
        if (wake_fd < 0) {
            return wait(want_read, want_write, timeout_ms);
        }
        pollfd p[2] = {
            {fd_, static_cast<short>((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0)), 0},
            {wake_fd, POLLIN, 0}};
        const int n = ::poll(p, 2, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                return {};
            }
            fail(path_ + ": poll");
        }
        // A hang-up counts as an error, so the caller asks the device and
        // fails instead of polling it again at once.
        Readiness r{(p[0].revents & POLLIN) != 0, (p[0].revents & POLLOUT) != 0,
                    (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0};
        r.woken = (p[1].revents & POLLIN) != 0;
        return r;
    }

private:
    std::string path_;
    int fd_;
};

}  // namespace

std::unique_ptr<Device> open_device(const std::string& path) {
    return std::make_unique<LinuxDevice>(path, false);
}

std::unique_ptr<Device> open_device_nonblocking(const std::string& path) {
    return std::make_unique<LinuxDevice>(path, true);
}

}  // namespace radioberry
