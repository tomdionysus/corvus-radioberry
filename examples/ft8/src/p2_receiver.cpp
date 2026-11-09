// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2_receiver.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <stdexcept>
#include <system_error>

namespace ft8 {
namespace {

// Ports as offsets from the base port: General 0, Receiver Specific 1,
// Transmitter Specific 2, High Priority 3; DDC0 I/Q comes from 11.
constexpr std::uint16_t kGeneral = 0;
constexpr std::uint16_t kRxSpecific = 1;
constexpr std::uint16_t kTxSpecific = 2;
constexpr std::uint16_t kHighPriority = 3;
constexpr std::uint16_t kDdc0 = 11;
constexpr std::size_t kShortBytes = 60;
constexpr std::size_t kLongBytes = 1444;
constexpr std::size_t kHeaderBytes = 16;
constexpr float kFullScale = 8388608.0f;  // 2^23
constexpr auto kHighPriorityPeriod = std::chrono::milliseconds(100);

std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 |
           std::uint32_t{p[3]};
}

void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

float s24(const std::uint8_t* p) {
    std::int32_t v = std::int32_t{p[0]} << 16 | std::int32_t{p[1]} << 8 | std::int32_t{p[2]};
    if ((v & 0x800000) != 0) {
        v -= 1 << 24;
    }
    return static_cast<float>(v) / kFullScale;
}

// The phase word for `hz` against the protocol's 122.88 MHz clock.
std::uint32_t phase_word(std::uint32_t hz) {
    return static_cast<std::uint32_t>(
        std::llround(static_cast<double>(hz) * 4294967296.0 / 122880000.0));
}

bool readable(int fd, std::chrono::milliseconds timeout) {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, static_cast<int>(timeout.count())) == 1;
}

}  // namespace

P2Receiver::P2Receiver(const std::string& host, std::uint16_t base_port) : base_port_(base_port) {
    radio_.sin_family = AF_INET;
    if (::inet_pton(AF_INET, host.c_str(), &radio_.sin_addr) != 1) {
        throw std::runtime_error("not an IPv4 address: " + host);
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "UDP socket");
    }
    const int buffer = 4 << 20;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
}

P2Receiver::~P2Receiver() {
    if (started_) {
        stop();
    }
    ::close(fd_);
}

void P2Receiver::send(std::uint16_t offset, const std::uint8_t* data, std::size_t size) {
    sockaddr_in to = radio_;
    to.sin_port = htons(static_cast<std::uint16_t>(base_port_ + offset));
    if (::sendto(fd_, data, size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) < 0) {
        throw std::system_error(errno, std::generic_category(), "send");
    }
}

std::optional<Receiver::Radio> P2Receiver::discover(std::chrono::milliseconds timeout) {
    std::uint8_t request[kShortBytes] = {0, 0, 0, 0, 0x02};
    send(kGeneral, request, sizeof request);
    std::uint8_t reply[2048];
    while (readable(fd_, timeout)) {
        const ssize_t n = ::recv(fd_, reply, sizeof reply, 0);
        if (n != static_cast<ssize_t>(kShortBytes) || be32(reply) != 0 ||
            (reply[4] != 2 && reply[4] != 3)) {
            continue;
        }
        Radio r;
        std::copy(reply + 5, reply + 11, r.mac.begin());
        r.board = reply[11];
        r.version_major = reply[13];
        r.receivers = reply[20];
        r.busy = reply[4] == 3;
        return r;
    }
    return std::nullopt;
}

void P2Receiver::start(std::uint32_t frequency_hz, int) {
    frequency_hz_ = frequency_hz;
    std::uint8_t general[kShortBytes] = {};
    general[37] = 0x08;  // frequencies as phase words
    send(kGeneral, general, sizeof general);
    std::uint8_t rx[kLongBytes] = {};
    rx[4] = 1;     // one ADC
    rx[7] = 0x01;  // DDC0 enabled
    rx[19] = 48;   // at 48 ksps
    rx[22] = 24;   // 24-bit samples
    send(kRxSpecific, rx, sizeof rx);
    std::uint8_t tx[kShortBytes] = {};
    tx[4] = 1;  // one DAC
    send(kTxSpecific, tx, sizeof tx);
    send_high_priority(true);
    started_ = true;
}

void P2Receiver::stop() {
    send_high_priority(false);
    started_ = false;
}

// Run as given, MOX never; DDC0 and the DUC on the frequency, drive 0.
void P2Receiver::send_high_priority(bool run) {
    std::uint8_t p[kLongBytes] = {};
    put_be32(p, high_priority_sequence_++);
    p[4] = run ? 0x01 : 0x00;
    put_be32(p + 9, phase_word(frequency_hz_));
    put_be32(p + 329, phase_word(frequency_hz_));
    send(kHighPriority, p, sizeof p);
    last_high_priority_ = std::chrono::steady_clock::now();
}

bool P2Receiver::receive(std::vector<std::complex<float>>& out, std::chrono::milliseconds timeout) {
    std::uint8_t p[2048];
    for (;;) {
        if (std::chrono::steady_clock::now() - last_high_priority_ >= kHighPriorityPeriod) {
            send_high_priority(true);
        }
        if (!readable(fd_, timeout)) {
            return false;
        }
        sockaddr_in from{};
        socklen_t len = sizeof from;
        const ssize_t n = ::recvfrom(fd_, p, sizeof p, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n != static_cast<ssize_t>(kLongBytes) ||
            ntohs(from.sin_port) != static_cast<std::uint16_t>(base_port_ + kDdc0)) {
            continue;  // status, Mic/Line or anything else
        }
        const std::uint32_t sequence = be32(p);
        if (last_sequence_ && sequence > *last_sequence_ + 1) {
            lost_ += sequence - *last_sequence_ - 1;
        }
        last_sequence_ = sequence;
        ++packets_;
        const std::size_t samples = std::size_t{p[14]} << 8 | p[15];
        const std::uint8_t* s = p + kHeaderBytes;
        for (std::size_t i = 0; i < samples && i < 238; ++i, s += 6) {
            out.emplace_back(swap_iq_ ? s24(s + 3) : s24(s), swap_iq_ ? s24(s) : s24(s + 3));
        }
        return true;
    }
}

}  // namespace ft8
