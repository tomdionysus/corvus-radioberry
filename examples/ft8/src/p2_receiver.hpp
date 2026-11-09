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

// A minimal openHPSDR Protocol 2 receive client over UDP: DDC0 at 48 ksps.
// It sends General, Receiver and Transmitter Specific and High Priority
// packets to the radio's base port and the ports after it, and repeats
// High Priority every 100 ms while receiving, which keeps the stream
// owned. Protocol 2 has no LNA gain for this radio: the server's own
// setting applies (radioberryd --p2-rx-gain).
class P2Receiver final : public Receiver {
public:
    P2Receiver(const std::string& host, std::uint16_t base_port);
    ~P2Receiver() override;
    P2Receiver(const P2Receiver&) = delete;
    P2Receiver& operator=(const P2Receiver&) = delete;

    std::optional<Radio> discover(std::chrono::milliseconds timeout) override;
    void start(std::uint32_t frequency_hz, int lna_gain_db) override;
    bool receive(std::vector<std::complex<float>>& out, std::chrono::milliseconds timeout) override;
    void stop() override;

private:
    void send(std::uint16_t offset, const std::uint8_t* data, std::size_t size);
    void send_high_priority(bool run);

    int fd_ = -1;
    sockaddr_in radio_{};
    std::uint16_t base_port_ = 0;
    std::uint32_t frequency_hz_ = 0;
    std::uint32_t high_priority_sequence_ = 0;
    std::chrono::steady_clock::time_point last_high_priority_{};
    std::optional<std::uint32_t> last_sequence_;
    bool started_ = false;
};

}  // namespace ft8
