// SPDX-License-Identifier: GPL-2.0-or-later
#include "p1_receiver.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace ft8 {
namespace {

constexpr std::size_t kPacketBytes = 1032;
constexpr std::size_t kHalfBytes = 512;
constexpr std::size_t kSamplesPerHalf = 63;  // one receiver: 8 bytes a sample
constexpr float kFullScale = 8388608.0f;     // 2^23

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

bool readable(int fd, std::chrono::milliseconds timeout) {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, static_cast<int>(timeout.count())) == 1;
}

}  // namespace

P1Receiver::P1Receiver(const std::string& host, std::uint16_t port) {
    radio_.sin_family = AF_INET;
    radio_.sin_port = htons(port);
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

P1Receiver::~P1Receiver() {
    if (started_) {
        try {
            stop();
        } catch (...) {  // NOLINT(bugprone-empty-catch): the socket closes regardless
        }
    }
    ::close(fd_);
}

void P1Receiver::send(const std::uint8_t* data, std::size_t size) {
    if (::sendto(fd_, data, size, 0, reinterpret_cast<const sockaddr*>(&radio_), sizeof radio_) <
        0) {
        throw std::system_error(errno, std::generic_category(), "send");
    }
}

std::optional<P1Receiver::Radio> P1Receiver::discover(std::chrono::milliseconds timeout) {
    std::uint8_t request[63] = {0xef, 0xfe, 0x02};
    send(request, sizeof request);
    std::uint8_t reply[2048];
    while (readable(fd_, timeout)) {
        const ssize_t n = ::recv(fd_, reply, sizeof reply, 0);
        if (n < 22 || reply[0] != 0xef || reply[1] != 0xfe || (reply[2] != 2 && reply[2] != 3)) {
            continue;
        }
        Radio r;
        std::copy(reply + 3, reply + 9, r.mac.begin());
        r.version_major = reply[9];
        r.board = reply[10];
        r.receivers = reply[19];
        r.version_minor = reply[21];
        r.busy = reply[2] == 3;
        return r;
    }
    return std::nullopt;
}

void P1Receiver::start(std::uint32_t frequency_hz, int lna_gain_db) {
    if (lna_gain_db < -12 || lna_gain_db > 48) {
        throw std::runtime_error("LNA gain outside -12 to 48 dB");
    }
    frequency_hz_ = frequency_hz;
    lna_word_ = 0x40u | static_cast<std::uint32_t>(lna_gain_db + 12);
    // The whole configuration goes out before the start.
    for (int i = 0; i < 2; ++i) {
        send_host_packet();
    }
    std::uint8_t start[64] = {0xef, 0xfe, 0x04, 0x01};
    send(start, sizeof start);
    started_ = true;
}

void P1Receiver::stop() {
    std::uint8_t stop[64] = {0xef, 0xfe, 0x04, 0x00};
    send(stop, sizeof stop);
    started_ = false;
}

// Two command blocks per packet, cycling through the configuration:
// 0x00 (48 ksps, one receiver, duplex), 0x02 (receiver 1), 0x01 (TX
// frequency, the same) and 0x0a (LNA gain). C0 bit 0, MOX, is never set.
void P1Receiver::send_host_packet() {
    std::uint8_t p[kPacketBytes] = {0xef, 0xfe, 0x01, 0x02};
    put_be32(p + 4, host_sequence_++);
    for (std::size_t half = 0; half < 2; ++half) {
        std::uint8_t* h = p + 8 + half * kHalfBytes;
        h[0] = h[1] = h[2] = 0x7f;
        std::uint8_t address = 0;
        std::uint32_t data = 0;
        switch (command_slot_++ % 4) {
            case 0:
                address = 0x00;
                data = 1u << 2;
                break;
            case 1:
                address = 0x02;
                data = frequency_hz_;
                break;
            case 2:
                address = 0x01;
                data = frequency_hz_;
                break;
            default:
                address = 0x0a;
                data = lna_word_;
                break;
        }
        h[3] = static_cast<std::uint8_t>(address << 1);
        put_be32(h + 4, data);
    }
    send(p, sizeof p);
}

bool P1Receiver::receive(std::vector<std::complex<float>>& out, std::chrono::milliseconds timeout) {
    std::uint8_t p[2048];
    for (;;) {
        if (!readable(fd_, timeout)) {
            return false;
        }
        const ssize_t n = ::recv(fd_, p, sizeof p, 0);
        if (n != static_cast<ssize_t>(kPacketBytes) || p[0] != 0xef || p[1] != 0xfe ||
            p[2] != 0x01 || p[3] != 0x06) {
            continue;
        }
        const std::uint32_t sequence = be32(p + 4);
        if (last_sequence_ && sequence > *last_sequence_ + 1) {
            lost_ += sequence - *last_sequence_ - 1;
        }
        last_sequence_ = sequence;
        ++packets_;
        for (std::size_t half = 0; half < 2; ++half) {
            const std::uint8_t* s = p + 8 + half * kHalfBytes + 8;
            for (std::size_t i = 0; i < kSamplesPerHalf; ++i, s += 8) {
                out.emplace_back(swap_iq_ ? s24(s + 3) : s24(s), swap_iq_ ? s24(s) : s24(s + 3));
            }
        }
        send_host_packet();
        return true;
    }
}

}  // namespace ft8
