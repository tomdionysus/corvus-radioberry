// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <netinet/in.h>

#include <array>
#include <chrono>
#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ft8 {

// A minimal openHPSDR Protocol 1 (Hermes-Lite 2 dialect) receive client
// over UDP: one receiver at 48 ksps. Receive only: it never sets MOX, and
// the TX samples it sends are zero. Failures throw std::system_error or
// std::runtime_error.
class P1Receiver {
public:
    struct Radio {
        std::array<std::uint8_t, 6> mac{};
        unsigned version_major = 0;
        unsigned version_minor = 0;
        unsigned board = 0;
        unsigned receivers = 0;
        bool busy = false;  // already streaming to another client
    };

    P1Receiver(const std::string& host, std::uint16_t port);
    ~P1Receiver();
    P1Receiver(const P1Receiver&) = delete;
    P1Receiver& operator=(const P1Receiver&) = delete;

    // Sends discovery and waits up to `timeout` for the reply.
    std::optional<Radio> discover(std::chrono::milliseconds timeout);
    // Configures one receiver at 48 ksps on `frequency_hz` with the LNA at
    // `lna_gain_db` (-12 to 48), then starts the stream.
    void start(std::uint32_t frequency_hz, int lna_gain_db);
    // Waits up to `timeout` for the next packet and appends its receiver
    // samples, I and Q scaled to +-1, to `out`. Answers each packet with a
    // host packet, which carries the configuration and keeps the stream
    // owned. False on timeout.
    bool receive(std::vector<std::complex<float>>& out, std::chrono::milliseconds timeout);
    void stop();

    // Takes each sample's second value as I (Q first).
    void set_swap_iq(bool swap) { swap_iq_ = swap; }

    std::uint64_t packets() const { return packets_; }
    // Packets missing from the radio's sequence.
    std::uint64_t lost() const { return lost_; }

private:
    void send(const std::uint8_t* data, std::size_t size);
    void send_host_packet();

    int fd_ = -1;
    sockaddr_in radio_{};
    std::uint32_t frequency_hz_ = 0;
    std::uint32_t lna_word_ = 0;
    std::uint32_t host_sequence_ = 0;
    unsigned command_slot_ = 0;
    std::optional<std::uint32_t> last_sequence_;
    std::uint64_t packets_ = 0;
    std::uint64_t lost_ = 0;
    bool started_ = false;
    bool swap_iq_ = false;
};

}  // namespace ft8
