// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests of radioberry.ko through /dev/radioberry on the Raspberry Pi with
// the Radioberry attached. Receive only: no command sets PTT or the TX
// permission bit, and the TX samples written are zero.
//
//   rbdrivertest [Catch2 options]
//   RADIOBERRY_DEVICE=/dev/other rbdrivertest
#include <fcntl.h>
#include <poll.h>
#include <radioberry_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "device/service.hpp"
#include "radioberry/device.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string device_path() {
    const char* env = std::getenv("RADIOBERRY_DEVICE");
    return env != nullptr ? env : "/dev/radioberry";
}

class Fd {
public:
    explicit Fd(int flags = 0) : fd_(::open(device_path().c_str(), O_RDWR | O_CLOEXEC | flags)) {}
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    void close() {
        ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_;
};

radioberry_command command(int fd, std::uint8_t address, std::uint32_t data) {
    radioberry_command c{};
    c.tx[0] = RADIOBERRY_CMD_RUN;
    c.tx[1] = static_cast<std::uint8_t>(address << 1);  // PTT clear
    c.tx[2] = static_cast<std::uint8_t>(data >> 24);
    c.tx[3] = static_cast<std::uint8_t>(data >> 16);
    c.tx[4] = static_cast<std::uint8_t>(data >> 8);
    c.tx[5] = static_cast<std::uint8_t>(data);
    REQUIRE(::ioctl(fd, RADIOBERRY_IOC_COMMAND, &c) == 0);
    return c;
}

// Rate code, receivers, duplex; then every receiver on 14.074 MHz.
void configure(int fd, unsigned rate_code, unsigned receivers) {
    command(fd, 0x00, rate_code << 24 | (receivers - 1) << 3 | 1u << 2);
    for (unsigned r = 0; r < receivers; ++r) {
        command(fd, static_cast<std::uint8_t>(0x02 + r), 14074000);
    }
}

radioberry_info info(int fd) {
    radioberry_info i{};
    REQUIRE(::ioctl(fd, RADIOBERRY_IOC_INFO, &i) == 0);
    return i;
}

radioberry_stats stats(int fd) {
    radioberry_stats s{};
    REQUIRE(::ioctl(fd, RADIOBERRY_IOC_STATS, &s) == 0);
    return s;
}

// Reads and discards for `d`.
void drain_for(int fd, std::size_t frame, Clock::duration d) {
    std::vector<std::uint8_t> buf(frame * 1000);
    for (const auto end = Clock::now() + d; Clock::now() < end;) {
        REQUIRE(::read(fd, buf.data(), buf.size()) > 0);
    }
}

std::string sysfs(const std::string& name) {
    std::ifstream in("/sys/bus/spi/devices/spi0.0/" + name);
    std::string value;
    std::getline(in, value);
    return value;
}

// The driver's stall count when the run started; the last test compares.
const std::string stalls_at_start = sysfs("rx_stalls");

}  // namespace

TEST_CASE("the device opens once at a time", "[open]") {
    Fd first;
    REQUIRE(first.get() >= 0);
    {
        Fd second;
        CHECK(second.get() < 0);
        CHECK(errno == EBUSY);
    }
    first.close();
    Fd third;
    CHECK(third.get() >= 0);
}

TEST_CASE("info agrees with sysfs", "[info]") {
    Fd fd;
    REQUIRE(fd.get() >= 0);
    const radioberry_info i = info(fd.get());
    CHECK(i.abi_version == RADIOBERRY_ABI_VERSION);
    CHECK(sysfs("gateware_version") ==
          std::to_string(i.gateware_major) + "." + std::to_string(i.gateware_minor));
    CHECK(sysfs("receivers") == std::to_string(i.receivers));
    CHECK(sysfs("transmitters") == std::to_string(i.transmitters));
    CHECK(sysfs("fpga_type") == (i.fpga_type == RADIOBERRY_FPGA_CL025 ? "10CL025" : "10CL016"));
    CHECK(i.receivers >= 1);
}

TEST_CASE("reads return whole frames for every receiver count", "[read]") {
    Fd fd;
    REQUIRE(fd.get() >= 0);
    const unsigned receivers = info(fd.get()).receivers;
    for (unsigned n = 1; n <= receivers; ++n) {
        INFO("receivers " << n);
        configure(fd.get(), 0, n);
        const radioberry_info i = info(fd.get());
        CHECK(i.active_receivers == n);
        CHECK(i.rx_rate_code == 0);
        const std::size_t frame = 6u * n;
        drain_for(fd.get(), frame, 50ms);

        const radioberry_stats before = stats(fd.get());
        // An odd buffer size: reads must stop at a frame boundary.
        std::vector<std::uint8_t> buf(frame * 7 + 5);
        std::size_t total = 0;
        for (const auto end = Clock::now() + 200ms; Clock::now() < end;) {
            const ssize_t got = ::read(fd.get(), buf.data(), buf.size());
            REQUIRE(got > 0);
            CHECK(static_cast<std::size_t>(got) % frame == 0);
            CHECK(static_cast<std::size_t>(got) <= frame * 7);
            total += static_cast<std::size_t>(got);
        }
        const radioberry_stats after = stats(fd.get());
        CHECK(after.rx_resyncs == before.rx_resyncs);
        CHECK(after.rx_dropped_words == before.rx_dropped_words);
        // 200 ms at 48 ksps is 9600 frames; allow for scheduling.
        CHECK(total / frame > 8000);

        errno = 0;
        CHECK(::read(fd.get(), buf.data(), frame - 1) == -1);
        CHECK(errno == EINVAL);
    }
}

TEST_CASE("non-blocking reads drain to EAGAIN", "[read]") {
    Fd fd(O_NONBLOCK);
    REQUIRE(fd.get() >= 0);
    configure(fd.get(), 0, 1);
    std::vector<std::uint8_t> buf(6 * 4096);
    bool again = false;
    for (int i = 0; i < 100000 && !again; ++i) {
        const ssize_t got = ::read(fd.get(), buf.data(), buf.size());
        if (got < 0) {
            REQUIRE(errno == EAGAIN);
            again = true;
        } else {
            CHECK(got % 6 == 0);
        }
    }
    CHECK(again);
}

TEST_CASE("poll reports frames, TX space and RX overrun", "[poll]") {
    Fd fd(O_NONBLOCK);
    REQUIRE(fd.get() >= 0);
    configure(fd.get(), 0, 1);

    pollfd p{fd.get(), POLLIN, 0};
    REQUIRE(::poll(&p, 1, 100) == 1);
    CHECK((p.revents & POLLIN) != 0);
    CHECK((p.revents & POLLERR) == 0);
    pollfd w{fd.get(), POLLOUT, 0};
    REQUIRE(::poll(&w, 1, 0) == 1);
    CHECK((w.revents & POLLOUT) != 0);

    // Three receivers at 384 ksps fill the RX ring in well under 300 ms.
    configure(fd.get(), 3, 3);
    std::this_thread::sleep_for(300ms);
    p.revents = 0;
    REQUIRE(::poll(&p, 1, 0) == 1);
    CHECK((p.revents & POLLERR) != 0);

    // Drain, then the statistics call reports and clears the overrun.
    std::vector<std::uint8_t> buf(18 * 4096);
    while (::read(fd.get(), buf.data(), buf.size()) > 0) {
    }
    REQUIRE(errno == EAGAIN);
    const radioberry_stats s = stats(fd.get());
    CHECK((s.flags & RADIOBERRY_STATS_RX_OVERRUN) != 0);
    CHECK(s.rx_overrun_frames > 0);
    p.revents = 0;
    ::poll(&p, 1, 0);
    CHECK((p.revents & POLLERR) == 0);
    CHECK((stats(fd.get()).flags & RADIOBERRY_STATS_RX_OVERRUN) == 0);
    configure(fd.get(), 0, 1);
}

TEST_CASE("short reads and partial TX samples are rejected", "[read][write]") {
    Fd fd;
    REQUIRE(fd.get() >= 0);
    configure(fd.get(), 0, 2);
    std::array<std::uint8_t, 16> buf{};
    errno = 0;
    CHECK(::read(fd.get(), buf.data(), 11) == -1);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(::write(fd.get(), buf.data(), 3) == -1);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(::write(fd.get(), buf.data(), 6) == -1);
    CHECK(errno == EINVAL);
    CHECK(::write(fd.get(), buf.data(), 0) == 0);
}

TEST_CASE("non-blocking writes fill the TX ring and it drains at 48 ksps", "[write]") {
    Fd fd(O_NONBLOCK);
    REQUIRE(fd.get() >= 0);
    configure(fd.get(), 0, 1);
    const std::vector<std::uint8_t> zeros(4096, 0);
    std::size_t written = 0;
    for (int i = 0; i < 1000; ++i) {
        const ssize_t n = ::write(fd.get(), zeros.data(), zeros.size());
        if (n < 0) {
            REQUIRE(errno == EAGAIN);
            break;
        }
        written += static_cast<std::size_t>(n);
    }
    const radioberry_stats full = stats(fd.get());
    CHECK(full.tx_ring_words > 0);
    CHECK(written >= 32 * 1024);
    CHECK((full.flags & RADIOBERRY_STATS_PTT) == 0);

    // The gateware's own 4096-sample FIFO first fills at the PIO bus rate,
    // about 102 000 words per second, for up to about 75 ms depending on
    // how full it was; after that the stream runs at 48 ksps. The full ring
    // holds 341 ms, so both steps fit inside it.
    std::this_thread::sleep_for(100ms);
    const radioberry_stats steady = stats(fd.get());
    CHECK(steady.tx_words - full.tx_words >= 4800 - 512);
    std::this_thread::sleep_for(100ms);
    const radioberry_stats later = stats(fd.get());
    const auto words = later.tx_words - steady.tx_words;
    // 0.1 s at 48 ksps, with two 256-sample DMA slots of slack each way.
    CHECK(words >= 4800 - 512);
    CHECK(words <= 4800 + 512);
    CHECK(later.tx_ring_words > 0);
    CHECK(later.tx_underruns == full.tx_underruns);
    // POLLOUT means at least one sample fits.
    pollfd p{fd.get(), POLLOUT, 0};
    CHECK(::poll(&p, 1, 0) == 1);
}

TEST_CASE("unknown ioctls fail with ENOTTY", "[ioctl]") {
    Fd fd;
    REQUIRE(fd.get() >= 0);
    errno = 0;
    CHECK(::ioctl(fd.get(), _IO(RADIOBERRY_IOC_MAGIC, 99)) == -1);
    CHECK(errno == ENOTTY);
}

TEST_CASE("after close the gateware reports no transmit state", "[close]") {
    {
        Fd fd;
        REQUIRE(fd.get() >= 0);
        configure(fd.get(), 0, 1);
    }
    Fd fd;
    REQUIRE(fd.get() >= 0);
    // The last command the gateware saw was the safe command sent on close.
    CHECK((stats(fd.get()).flags & RADIOBERRY_STATS_PTT) == 0);
    const radioberry_command reply = command(fd.get(), 0x0a, 0x40 | 32);
    CHECK((reply.rx[0] & 0x01) == 0);  // cw_on and external PTT clear
    CHECK(reply.rx[4] == info(fd.get()).gateware_major);
}

TEST_CASE("the daemon's device layer streams through the driver", "[service]") {
    namespace rd = radioberry::daemon;
    std::mutex mutex;
    std::vector<rd::Fault> faults;
    const auto device = radioberry::open_device_nonblocking(device_path());
    rd::DeviceService service(*device, [&](const rd::Fault& f) {
        const std::lock_guard lock(mutex);
        faults.push_back(f);
    });
    REQUIRE(service.start());
    service.control().set(radioberry::hl2::general(0, 1));
    service.control().set(radioberry::hl2::rx_frequency(0, 14074000));
    // Let the receiver-count command and the driver's settling pass.
    for (const auto end = Clock::now() + 100ms; Clock::now() < end;) {
        while (service.rx().front() != nullptr) {
            service.rx().pop();
        }
        std::this_thread::sleep_for(1ms);
    }

    // One second: count frames, keep zero TX samples queued (PTT clear).
    std::size_t frames = 0;
    std::uint64_t last_sequence = 0;
    bool gap = false;
    for (const auto end = Clock::now() + 1s; Clock::now() < end;) {
        while (const rd::RxBlock* b = service.rx().front()) {
            CHECK(b->receivers == 1);
            gap = gap || (last_sequence != 0 && b->sequence != last_sequence + 1);
            last_sequence = b->sequence;
            frames += b->frames();
            service.rx().pop();
        }
        if (rd::TxBlock* t = service.tx().acquire()) {
            t->bytes.fill(0);
            t->size = rd::TxBlock::kCapacity;
            service.tx().publish();
        }
        std::this_thread::sleep_for(1ms);
    }
    const rd::DeviceSnapshot s = service.snapshot();
    service.stop();

    CHECK_FALSE(gap);
    CHECK(frames > 47000);
    CHECK(frames < 49000);
    CHECK(s.rx_blocks_dropped == 0);
    CHECK(s.tx_bytes > 0);
    CHECK_FALSE(s.failed);
    CHECK_FALSE(s.stalled);
    REQUIRE(s.driver.has_value());
    CHECK((s.driver->flags & RADIOBERRY_STATS_PTT) == 0);
    const std::lock_guard lock(mutex);
    for (const rd::Fault& f : faults) {
        UNSCOPED_INFO(rd::describe(f));
    }
    CHECK(faults.empty());
}

// Declared last so it runs last (Catch2 runs in declaration order).
TEST_CASE("the RX stream never stalled during the run", "[stall]") {
    REQUIRE_FALSE(stalls_at_start.empty());
    CHECK(sysfs("rx_stalled") == "0");
    CHECK(sysfs("rx_stalls") == stalls_at_start);
}
