// SPDX-License-Identifier: GPL-2.0-or-later
//
// Protocol 2 (openHPSDR Ethernet Protocol v4): discovery and General on
// the base port, Receiver Specific (+1), Transmitter Specific (+2), High
// Priority (+3) and TX I/Q (+5) from the client; High Priority status
// (+1), Mic/Line (+2) and DDC n I/Q (+11 + n) from the radio. Frequencies
// are sent as phase words.
#include <cmath>
#include <stdexcept>
#include <vector>

#include "client.hpp"

namespace radioberry::proto {
namespace {

constexpr unsigned kGeneral = 0;
constexpr unsigned kRxSpecific = 1;
constexpr unsigned kTxSpecific = 2;
constexpr unsigned kHighPriority = 3;
constexpr unsigned kTxIq = 5;
constexpr unsigned kStatus = 1;
constexpr unsigned kMicLine = 2;
constexpr unsigned kDdc0 = 11;
constexpr std::size_t kShortBytes = 60;
constexpr std::size_t kLongBytes = 1444;
constexpr std::size_t kMicBytes = 132;
constexpr std::size_t kDdcHeaderBytes = 16;
constexpr std::size_t kDdcSamplesPerPacket = 238;
constexpr std::size_t kMicSamplesPerPacket = 64;
constexpr std::size_t kTxSamplesPerPacket = 240;
constexpr unsigned kMaxDdcs = 8;
constexpr std::uint32_t kTxRateHz = 192000;
constexpr std::uint32_t kMicRateHz = 48000;
constexpr auto kHighPriorityPeriod = std::chrono::milliseconds(100);

// The phase word for `hz` against the protocol's 122.88 MHz clock.
std::uint32_t phase_word(std::uint32_t hz) {
    return static_cast<std::uint32_t>(
        std::llround(static_cast<double>(hz) * 4294967296.0 / 122880000.0));
}

class P2Client final : public Client {
public:
    P2Client(Transport& transport, Clock now) : transport_(transport), now_(std::move(now)) {}

    ~P2Client() override {
        if (started_) {
            try {
                stop();
            } catch (...) {
            }
        }
    }

    std::optional<Radio> discover(std::chrono::milliseconds timeout) override {
        std::vector<std::uint8_t> request(kShortBytes, 0);
        request[4] = 0x02;
        transport_.send(kGeneral, request);
        while (auto d = transport_.receive(timeout)) {
            const auto& r = d->bytes;
            if (d->offset != kGeneral || r.size() != kShortBytes || be32(r, 0) != 0 ||
                (r[4] != 0x02 && r[4] != 0x03)) {
                continue;
            }
            Radio radio;
            std::copy(r.begin() + 5, r.begin() + 11, radio.mac.begin());
            radio.board = r[11];
            radio.protocol_version = r[12];
            radio.version_major = r[13];
            radio.receivers = r[20];
            radio.busy = r[4] == 0x03;
            return radio;
        }
        return std::nullopt;
    }

    void start(const StreamConfig& config) override {
        if (config.receivers < 1 || config.receivers > kMaxDdcs) {
            throw std::runtime_error("this client takes 1 to 8 DDCs");
        }
        if (config.rate_ksps != 48 && config.rate_ksps != 96 && config.rate_ksps != 192 &&
            config.rate_ksps != 384) {
            throw std::runtime_error("DDC rates are 48, 96, 192 or 384 ksps");
        }
        if (config.lna_db) {
            throw std::runtime_error("Protocol 2 carries no LNA gain for this radio");
        }
        config_ = config;
        counts_.levels.assign(config.receivers, Levels{});

        std::vector<std::uint8_t> general(kShortBytes, 0);
        general[37] = 0x08;  // frequencies as phase words
        transport_.send(kGeneral, general);

        std::vector<std::uint8_t> rx(kLongBytes, 0);
        rx[4] = 1;  // one ADC
        for (unsigned n = 0; n < config.receivers; ++n) {
            rx[7] = static_cast<std::uint8_t>(rx[7] | 1u << n);
            rx[18 + 6 * n] = static_cast<std::uint8_t>(config.rate_ksps >> 8);
            rx[19 + 6 * n] = static_cast<std::uint8_t>(config.rate_ksps);
            rx[22 + 6 * n] = 24;
        }
        transport_.send(kRxSpecific, rx);

        std::vector<std::uint8_t> tx(kShortBytes, 0);
        tx[4] = 1;  // one DAC
        tx[14] = 0;
        tx[15] = 192;  // DUC at 192 ksps
        tx[16] = 24;
        transport_.send(kTxSpecific, tx);

        send_high_priority(true);
        started_ = true;
        mic_samples_ = 0;
        tx_iq_sent_ = 0;
    }

    void set_mox(bool mox) override {
        if (mox != mox_) {
            mox_ = mox;
            mic_samples_ = 0;
            tx_iq_sent_ = 0;
            send_high_priority(true);
        }
    }

    bool poll(std::chrono::milliseconds timeout) override {
        for (;;) {
            if (now_() - last_high_priority_ >= kHighPriorityPeriod) {
                send_high_priority(true);
            }
            auto d = transport_.receive(timeout);
            if (!d) {
                return false;
            }
            const auto& p = d->bytes;
            if (d->offset == kStatus && p.size() == kShortBytes) {
                if ((p[4] & 0x01) != 0) {
                    ++counts_.ptt_reported;
                }
            } else if (d->offset == kMicLine && p.size() == kMicBytes) {
                mic_samples_ += kMicSamplesPerPacket;
                send_tx_iq_owed();
            } else if (d->offset >= kDdc0 && d->offset < kDdc0 + config_.receivers &&
                       p.size() == kLongBytes) {
                take(d->offset - kDdc0, p);
                return true;
            }
        }
    }

    void stop() override {
        mox_ = false;
        send_high_priority(false);
        started_ = false;
    }

    double expected_packets_per_second() const override {
        return config_.receivers * config_.rate_ksps * 1000.0 / kDdcSamplesPerPacket;
    }

private:
    void take(unsigned ddc, const std::vector<std::uint8_t>& p) {
        count_sequence(ddc, be32(p, 0));
        ++counts_.packets;
        const std::size_t samples =
            std::min<std::size_t>(std::size_t{p[14]} << 8 | p[15], kDdcSamplesPerPacket);
        for (std::size_t s = 0; s < samples; ++s) {
            const std::size_t at = kDdcHeaderBytes + 6 * s;
            counts_.levels[ddc].add(s24(p, at), s24(p, at + 3));
        }
    }

    // While MOX is held, zero TX I/Q at 192 ksps, clocked by the radio's
    // Mic/Line packets at 48 ksps, as piHPSDR does.
    void send_tx_iq_owed() {
        if (!mox_) {
            return;
        }
        const std::uint64_t owed = mic_samples_ * (kTxRateHz / kMicRateHz) / kTxSamplesPerPacket;
        while (tx_iq_sent_ < owed) {
            std::vector<std::uint8_t> p(kLongBytes, 0);
            put_be32(p, 0, tx_iq_sequence_++);
            transport_.send(kTxIq, p);
            ++tx_iq_sent_;
            ++counts_.tx_packets;
        }
    }

    // Run as given, MOX as set; every DDC and the DUC on the frequency;
    // drive 0.
    void send_high_priority(bool run) {
        std::vector<std::uint8_t> p(kLongBytes, 0);
        put_be32(p, 0, high_priority_sequence_++);
        p[4] = static_cast<std::uint8_t>((run ? 0x01 : 0) | (mox_ ? 0x02 : 0));
        const std::uint32_t phase = phase_word(config_.frequency_hz);
        for (unsigned n = 0; n < config_.receivers; ++n) {
            put_be32(p, 9 + 4 * n, phase);
        }
        put_be32(p, 329, phase);
        p[345] = 0;  // drive
        transport_.send(kHighPriority, p);
        last_high_priority_ = now_();
    }

    Transport& transport_;
    Clock now_;
    StreamConfig config_;
    std::chrono::nanoseconds last_high_priority_{};
    std::uint32_t high_priority_sequence_ = 0;
    std::uint32_t tx_iq_sequence_ = 0;
    std::uint64_t mic_samples_ = 0;
    std::uint64_t tx_iq_sent_ = 0;
    bool mox_ = false;
    bool started_ = false;
};

}  // namespace

std::unique_ptr<Client> make_p2_client(Transport& transport, Clock now) {
    return std::make_unique<P2Client>(transport, std::move(now));
}

}  // namespace radioberry::proto
