// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <complex>
#include <cstdint>
#include <optional>
#include <vector>

namespace ft8 {

// One receive channel at 48 ksps from an openHPSDR radio. Receive only:
// it never sets MOX. Failures throw std::system_error or
// std::runtime_error.
class Receiver {
public:
    struct Radio {
        std::array<std::uint8_t, 6> mac{};
        unsigned version_major = 0;
        unsigned version_minor = 0;
        unsigned board = 0;
        unsigned receivers = 0;
        bool busy = false;  // already streaming to another client
    };

    virtual ~Receiver() = default;

    // Sends discovery and waits up to `timeout` for the reply.
    virtual std::optional<Radio> discover(std::chrono::milliseconds timeout) = 0;
    // Tunes to `frequency_hz` with the LNA at `lna_gain_db` (-12 to 48;
    // where the protocol cannot set it, nothing) and starts the stream.
    virtual void start(std::uint32_t frequency_hz, int lna_gain_db) = 0;
    // Waits up to `timeout` for the next packet and appends its samples, I
    // and Q scaled to +-1, to `out`. False on timeout.
    virtual bool receive(std::vector<std::complex<float>>& out,
                         std::chrono::milliseconds timeout) = 0;
    virtual void stop() = 0;

    // Takes each sample's second value as I (Q first).
    void set_swap_iq(bool swap) { swap_iq_ = swap; }

    std::uint64_t packets() const { return packets_; }
    // Packets missing from the radio's sequence.
    std::uint64_t lost() const { return lost_; }

protected:
    bool swap_iq_ = false;
    std::uint64_t packets_ = 0;
    std::uint64_t lost_ = 0;
};

}  // namespace ft8
