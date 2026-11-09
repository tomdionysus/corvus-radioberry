// SPDX-License-Identifier: GPL-2.0-or-later
#include "live_source.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <poll.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gpiod.hpp>
#include <sstream>

extern "C" {
#include <i2c/smbus.h>
}

namespace radioberry::probe {
namespace fs = std::filesystem;

namespace {

constexpr int kCommandTimeoutMs = 5000;

std::optional<fs::path> rp1_chip() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("gpiochip", 0) != 0 || entry.is_symlink(ec)) {
            continue;
        }
        try {
            gpiod::chip chip(entry.path());
            if (chip.get_info().label() == "pinctrl-rp1") {
                return entry.path();
            }
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): not a usable chip
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> LiveSource::read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return std::nullopt;
    }
    return buffer.str();
}

bool LiveSource::exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(path, ec)) && !ec;
}

std::vector<std::string> LiveSource::list_dir(const std::string& path) {
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(path, ec);
    if (ec) {
        return names;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        names.push_back(it->path().filename().string());
    }
    return names;
}

std::optional<std::string> LiveSource::read_link(const std::string& path) {
    std::error_code ec;
    const fs::path target = fs::read_symlink(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return target.string();
}

std::optional<CommandResult> LiveSource::run(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return std::nullopt;
    }
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return std::nullopt;
    }
    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        const int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) {
            args.push_back(const_cast<char*>(a.c_str()));
        }
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(pipe_fds[1]);

    std::string output;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kCommandTimeoutMs);
    bool timed_out = false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            timed_out = true;
            break;
        }
        pollfd pfd{pipe_fds[0], POLLIN, 0};
        const int ready = poll(&pfd, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            timed_out = ready == 0;
            break;
        }
        char buf[4096];
        const ssize_t n = read(pipe_fds[0], buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        output.append(buf, static_cast<std::size_t>(n));
    }
    close(pipe_fds[0]);
    if (timed_out) {
        kill(pid, SIGKILL);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timed_out) {
        spdlog::debug("command {} timed out", argv[0]);
        return std::nullopt;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        return std::nullopt;  // not found or not executable
    }
    return CommandResult{WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

ConfigStatusLines LiveSource::read_config_status() {
    ConfigStatusLines lines;
    const auto path = rp1_chip();
    if (!path) {
        lines.consumer = "no RP1 GPIO chip";
        return lines;
    }
    try {
        gpiod::chip chip(*path);
        for (const unsigned offset : {ConfigLines::kConfDone, ConfigLines::kNStatus}) {
            const auto info = chip.get_line_info(offset);
            if (info.used()) {
                lines.consumer = info.consumer().empty() ? "kernel" : info.consumer();
                return lines;
            }
        }
        auto request = chip.prepare_request()
                           .set_consumer("rbprobe")
                           .add_line_settings(
                               gpiod::line::offsets{ConfigLines::kConfDone, ConfigLines::kNStatus},
                               gpiod::line_settings().set_direction(gpiod::line::direction::INPUT))
                           .do_request();
        lines.conf_done = request.get_value(ConfigLines::kConfDone) == gpiod::line::value::ACTIVE;
        lines.nstatus = request.get_value(ConfigLines::kNStatus) == gpiod::line::value::ACTIVE;
        lines.available = true;
    } catch (const std::exception& e) {
        lines.consumer = e.what();
    }
    return lines;
}

std::optional<std::vector<I2cDevice>> LiveSource::scan_i2c(int bus) {
    const std::string path = "/dev/i2c-" + std::to_string(bus);
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }
    std::vector<I2cDevice> devices;
    // Same probing as i2cdetect's default mode: a read on address ranges
    // where a quick write could corrupt an EEPROM, a quick write elsewhere.
    for (int address = 0x08; address <= 0x77; ++address) {
        if (ioctl(fd, I2C_SLAVE, address) < 0) {
            if (errno == EBUSY) {
                devices.push_back({static_cast<std::uint8_t>(address), true});
            }
            continue;
        }
        const bool read_probe =
            (address >= 0x30 && address <= 0x37) || (address >= 0x50 && address <= 0x5f);
        const int result =
            read_probe ? i2c_smbus_read_byte(fd) : i2c_smbus_write_quick(fd, I2C_SMBUS_WRITE);
        if (result >= 0) {
            devices.push_back({static_cast<std::uint8_t>(address), false});
        }
    }
    close(fd);
    return devices;
}

ConfigResult LiveSource::configure_fpga(std::span<const std::uint8_t> image) {
    auto pins = open_gpiod_config_pins();
    return configure_passive_serial(*pins, image);
}

}  // namespace radioberry::probe
