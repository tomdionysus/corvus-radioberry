// SPDX-License-Identifier: GPL-2.0-or-later
//
// Hardware glue for rbsmi: the BCM2711's SMI registers through /dev/mem,
// read by polling the FIFO, and the gateware's SPI port through spidev.
// The kernel's bcm2835_smi driver, loaded by the rbsmi-measure overlay,
// keeps the SMI clock (125 MHz) and the pin functions; its /dev/smi is not
// used, since it reads only 8 and 16 bits wide and the RX bus needs SD17.
// Not unit tested; the logic is in app.cpp and driver/kernel/rb_smi.h.
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

namespace {

using radioberry::smi::Control;
using radioberry::smi::Reader;
using radioberry::smi::Timing;

// The SMI block on the BCM2711 (Pi 4 peripherals at 0xfe000000). Register
// layout as the Raspberry Pi kernel's bcm2835_smi.h names it; the field
// positions are those its driver and published SMI notes use.
constexpr off_t kSmiBase = 0xfe600000;
constexpr std::size_t kMapBytes = 4096;
constexpr std::size_t kSmiCs = 0x00 / 4;    // control and status
constexpr std::size_t kSmiL = 0x04 / 4;     // transfer length
constexpr std::size_t kSmiA = 0x08 / 4;     // address
constexpr std::size_t kSmiD = 0x0c / 4;     // data FIFO
constexpr std::size_t kSmiDsr0 = 0x10 / 4;  // device 0 read settings
constexpr std::size_t kSmiDc = 0x30 / 4;    // DMA control
constexpr std::uint32_t kCsEnable = 1u << 0;
constexpr std::uint32_t kCsDone = 1u << 1;
constexpr std::uint32_t kCsStart = 1u << 3;
constexpr std::uint32_t kCsClear = 1u << 4;
constexpr std::uint32_t kCsRxd = 1u << 29;  // the FIFO holds data
// RWIDTH (bits 31..30) 2: 18-bit reads, SD0..SD17 in each word (measured:
// SD17 is seen with 2 and nothing with 3).
constexpr std::uint32_t kWidth18 = 2;
// Polls of an empty FIFO before a read counts as stalled: far longer than
// any strobe at the slowest timing (254 cycles, 2 us).
constexpr unsigned long kStallPolls = 50'000'000;

std::string errno_text(const std::string& what) {
    return what + ": " + std::system_category().message(errno);
}

class SmiReader final : public Reader {
public:
    SmiReader() {
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error(errno_text("cannot open /dev/mem"));
        }
        void* map = ::mmap(nullptr, kMapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, kSmiBase);
        if (map == MAP_FAILED) {
            const std::string why = errno_text("cannot map the SMI registers");
            ::close(fd_);
            throw std::runtime_error(why);
        }
        regs_ = static_cast<volatile std::uint32_t*>(map);
        saved_dsr0_ = regs_[kSmiDsr0];
        saved_dc_ = regs_[kSmiDc];
    }
    ~SmiReader() override {
        regs_[kSmiCs] = 0;
        regs_[kSmiDsr0] = saved_dsr0_;
        regs_[kSmiDc] = saved_dc_;
        ::munmap(const_cast<std::uint32_t*>(regs_), kMapBytes);
        ::close(fd_);
    }
    SmiReader(const SmiReader&) = delete;
    SmiReader& operator=(const SmiReader&) = delete;

    void configure(const Timing& t) override {
        regs_[kSmiCs] = 0;
        regs_[kSmiDc] = 0;  // no DMA: the FIFO is polled
        regs_[kSmiA] = 0;
        regs_[kSmiDsr0] = kWidth18 << 30 | t.setup << 24 | t.hold << 16 | t.pace << 8 | t.strobe;
    }

    std::size_t read(std::span<std::uint32_t> words) override {
        regs_[kSmiCs] = 0;
        regs_[kSmiL] = static_cast<std::uint32_t>(words.size());
        regs_[kSmiCs] = kCsEnable | kCsClear;
        regs_[kSmiCs] = kCsEnable | kCsStart;
        std::size_t n = 0;
        unsigned long polls = 0;
        while (n < words.size()) {
            if (regs_[kSmiCs] & kCsRxd) {
                words[n++] = regs_[kSmiD];
                polls = 0;
            } else if (++polls == kStallPolls) {
                throw std::runtime_error("SMI read stalled after " + std::to_string(n) + " words");
            }
        }
        while (!(regs_[kSmiCs] & kCsDone)) {
        }
        return n;
    }

private:
    int fd_ = -1;
    volatile std::uint32_t* regs_ = nullptr;
    std::uint32_t saved_dsr0_ = 0;
    std::uint32_t saved_dc_ = 0;
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
    radioberry::smi::Environment env{
        .compatible = read_compatible,
        .driver_bound = [] { return std::filesystem::exists("/dev/radioberry"); },
        .open_reader = []() -> std::unique_ptr<Reader> { return std::make_unique<SmiReader>(); },
        .open_control = [](const std::string& device) -> std::unique_ptr<Control> {
            return std::make_unique<SpiControl>(device);
        },
        .now = [] { return std::chrono::steady_clock::now().time_since_epoch(); },
        .sleep = [](std::chrono::microseconds d) { std::this_thread::sleep_for(d); },
        .cpu_seconds = cpu_seconds,
        .out = std::cout,
        .err = std::cerr,
    };
    return radioberry::smi::run_app(args, env);
}
