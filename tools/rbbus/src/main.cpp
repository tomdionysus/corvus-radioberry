// SPDX-License-Identifier: GPL-2.0-or-later
//
// Hardware glue for rbbus: the BCM2711 GPIO registers through /dev/gpiomem
// and the gateware's SPI port through spidev. Not unit tested; the logic
// is in app.cpp and driver/kernel/rb_bitbang.h.
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "app.hpp"

// BCM2711 GPIO block, as /dev/gpiomem maps it (BCM2711 peripherals
// datasheet, chapter 5).
struct rb_bb_io {
    volatile std::uint32_t* regs;
};
namespace {
constexpr std::size_t kGpset0 = 0x1c / 4;
constexpr std::size_t kGpclr0 = 0x28 / 4;
constexpr std::size_t kGplev0 = 0x34 / 4;
constexpr std::size_t kPullCntrl0 = 0xe4 / 4;  // GPIO_PUP_PDN_CNTRL_REG0, two bits a pin
constexpr std::size_t kMapBytes = 4096;
}  // namespace
static inline void rb_bb_set(struct rb_bb_io* io, std::uint32_t mask) { io->regs[kGpset0] = mask; }
static inline void rb_bb_clr(struct rb_bb_io* io, std::uint32_t mask) { io->regs[kGpclr0] = mask; }
static inline std::uint32_t rb_bb_lev(struct rb_bb_io* io) { return io->regs[kGplev0]; }

#include "rb_bitbang.h"

namespace {

using radioberry::bus::Bus;
using radioberry::bus::BusCounters;
using radioberry::bus::Control;

std::string errno_text(const std::string& what) {
    return what + ": " + std::system_category().message(errno);
}

class Bcm2711Bus final : public Bus {
public:
    Bcm2711Bus() {
        fd_ = ::open("/dev/gpiomem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error(errno_text("cannot open /dev/gpiomem"));
        }
        void* map = ::mmap(nullptr, kMapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (map == MAP_FAILED) {
            const std::string why = errno_text("cannot map /dev/gpiomem");
            ::close(fd_);
            throw std::runtime_error(why);
        }
        io_.regs = static_cast<volatile std::uint32_t*>(map);
        bus_.io = &io_;
        for (unsigned i = 0; i < kSaved; ++i) {
            saved_[i] = io_.regs[i];  // GPFSEL0..2: function of GPIO 0..29
        }
        saved_pulls_[0] = io_.regs[kPullCntrl0];
        saved_pulls_[1] = io_.regs[kPullCntrl0 + 1];

        // Outputs low before they drive, then inputs pulled down, as the
        // Pi 5 driver sets them.
        rb_bb_clr(&io_, RB_BB_TX_CLK | RB_BB_TX_DATA | RB_BB_RX_CLK);
        for (const unsigned pin : {4u, 5u, 6u}) {
            set_function(pin, 1);
        }
        for (const unsigned pin : {12u, 18u, 19u, 20u, 21u, 25u}) {
            set_function(pin, 0);
            set_pull(pin, 2);
        }
    }
    ~Bcm2711Bus() override {
        rb_bb_clr(&io_, RB_BB_TX_CLK | RB_BB_TX_DATA | RB_BB_RX_CLK);
        for (unsigned i = 0; i < kSaved; ++i) {
            io_.regs[i] = saved_[i];
        }
        io_.regs[kPullCntrl0] = saved_pulls_[0];
        io_.regs[kPullCntrl0 + 1] = saved_pulls_[1];
        ::munmap(const_cast<std::uint32_t*>(io_.regs), kMapBytes);
        ::close(fd_);
    }
    Bcm2711Bus(const Bcm2711Bus&) = delete;
    Bcm2711Bus& operator=(const Bcm2711Bus&) = delete;

    unsigned rx_run(std::uint32_t* out, unsigned max_words, unsigned max_idle) override {
        return rb_bb_rx_run(&bus_, out, max_words, max_idle);
    }
    unsigned tx_run(const std::uint32_t* words, unsigned count, unsigned max_polls) override {
        return rb_bb_tx_run(&bus_, words, count, max_polls);
    }
    void set_settle_reads(unsigned reads) override { bus_.settle_reads = reads; }
    BusCounters counters() const override {
        const rb_bb_stats& s = bus_.stats;
        return {s.rx_clocks, s.rx_idle_clocks, s.rx_words,
                s.tx_clocks, s.tx_ready_waits, s.tx_words};
    }

private:
    static constexpr unsigned kSaved = 3;

    // GPFSELn: three bits a pin, 0 input, 1 output.
    void set_function(unsigned pin, std::uint32_t fn) {
        volatile std::uint32_t& reg = io_.regs[pin / 10];
        const unsigned shift = (pin % 10) * 3;
        reg = (reg & ~(7u << shift)) | (fn << shift);
    }
    // 0 none, 1 up, 2 down.
    void set_pull(unsigned pin, std::uint32_t pull) {
        volatile std::uint32_t& reg = io_.regs[kPullCntrl0 + pin / 16];
        const unsigned shift = (pin % 16) * 2;
        reg = (reg & ~(3u << shift)) | (pull << shift);
    }

    int fd_ = -1;
    rb_bb_io io_{};
    rb_bb_bus bus_{};
    std::uint32_t saved_[kSaved]{};
    std::uint32_t saved_pulls_[2]{};
};

// SPI mode 3, 12.5 MHz, as the overlay gives the driver.
class SpiControl final : public Control {
public:
    explicit SpiControl(const std::string& device) {
        fd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error(errno_text("cannot open " + device));
        }
        std::uint8_t mode = SPI_MODE_3;
        std::uint8_t bits = 8;
        std::uint32_t speed = kSpeedHz;
        if (::ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
            ::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
            ::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
            const std::string why = errno_text("cannot configure " + device);
            ::close(fd_);
            throw std::runtime_error(why);
        }
    }
    ~SpiControl() override { ::close(fd_); }
    SpiControl(const SpiControl&) = delete;
    SpiControl& operator=(const SpiControl&) = delete;

    std::array<std::uint8_t, 6> transfer(const std::array<std::uint8_t, 6>& tx) override {
        std::array<std::uint8_t, 6> rx{};
        spi_ioc_transfer t{};
        t.tx_buf = reinterpret_cast<std::uintptr_t>(tx.data());
        t.rx_buf = reinterpret_cast<std::uintptr_t>(rx.data());
        t.len = static_cast<std::uint32_t>(tx.size());
        t.speed_hz = kSpeedHz;
        t.bits_per_word = 8;
        if (::ioctl(fd_, SPI_IOC_MESSAGE(1), &t) < 0) {
            throw std::runtime_error(errno_text("SPI transfer failed"));
        }
        return rx;
    }

private:
    static constexpr std::uint32_t kSpeedHz = 12'500'000;
    int fd_ = -1;
};

std::string read_compatible() {
    std::ifstream in("/proc/device-tree/compatible", std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

double cpu_seconds() {
    rusage u{};
    ::getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
           static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    radioberry::bus::Environment env{
        .compatible = read_compatible,
        .driver_bound = [] { return std::filesystem::exists("/dev/radioberry"); },
        .open_bus = [] { return std::make_unique<Bcm2711Bus>(); },
        .open_control = [](const std::string& device) -> std::unique_ptr<Control> {
            return std::make_unique<SpiControl>(device);
        },
        .now = [] { return std::chrono::steady_clock::now().time_since_epoch(); },
        .sleep = [](std::chrono::microseconds d) { std::this_thread::sleep_for(d); },
        .cpu_seconds = cpu_seconds,
        .out = std::cout,
        .err = std::cerr,
    };
    return radioberry::bus::run_app(args, env);
}
