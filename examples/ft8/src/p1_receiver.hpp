// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <netinet/in.h>

#include <chrono>
#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "receiver.hpp"

namespace ft8 {

// A minimal openHPSDR Protocol 1 (Hermes-Lite 2 dialect) receive client
// over UDP: one receiver at 48 ksps. The TX samples it sends are zero.
// Each packet received is answered with a host packet, which carries the
// configuration and keeps the stream owned.
class P1Receiver final : public Receiver {
public:
    P1Receiver(const std::string& host, std::uint16_t port);
    ~P1Receiver() override;
    P1Receiver(const P1Receiver&) = delete;
    P1Receiver& operator=(const P1Receiver&) = delete;

    std::optional<Radio> discover(std::chrono::milliseconds timeout) override;
    void start(std::uint32_t frequency_hz, int lna_gain_db) override;
    bool receive(std::vector<std::complex<float>>& out, std::chrono::milliseconds timeout) override;
    void stop() override;

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
    bool started_ = false;
};

}  // namespace ft8
