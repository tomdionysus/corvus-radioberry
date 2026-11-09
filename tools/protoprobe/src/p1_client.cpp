// SPDX-License-Identifier: GPL-2.0-or-later
//
// Protocol 1, Hermes-Lite 2 dialect: discovery (EF FE 02), start and stop
// (EF FE 04), 1032-byte host packets carrying two C0..C4 command blocks
// and 126 TX samples, 1032-byte radio packets carrying two status blocks
// and the receivers' I/Q.
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

#include "client.hpp"

namespace radioberry::proto {
namespace {

constexpr std::size_t kPacketBytes = 1032;
constexpr std::size_t kHalfBytes = 512;
constexpr std::size_t kHalfPayload = 504;
constexpr std::size_t kHostSamplesPerPacket = 126;  // 63 per half, 8 bytes each
constexpr double kTxRateHz = 48000;

struct CommandWord {
    std::uint8_t address;
    std::uint32_t data;
};

unsigned rate_code(unsigned rate_ksps) {
    switch (rate_ksps) {
        case 48:
            return 0;
        case 96:
            return 1;
        case 192:
            return 2;
        case 384:
            return 3;
        default:
            throw std::runtime_error("Protocol 1 rates are 48, 96, 192 or 384 ksps");
    }
}

class P1Client final : public Client {
public:
    explicit P1Client(Transport& transport) : transport_(transport) {}

    ~P1Client() override {
        if (started_) {
            try {
                stop();
            } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor must not throw
            }
        }
    }

    std::optional<Radio> discover(std::chrono::milliseconds timeout) override {
        std::vector<std::uint8_t> request(63, 0);
        request[0] = 0xef;
        request[1] = 0xfe;
        request[2] = 0x02;
        transport_.send(0, request);
        while (auto d = transport_.receive(timeout)) {
            const auto& r = d->bytes;
            if (d->offset != 0 || r.size() < 22 || r[0] != 0xef || r[1] != 0xfe ||
                (r[2] != 0x02 && r[2] != 0x03)) {
                continue;
            }
            Radio radio;
            std::copy(r.begin() + 3, r.begin() + 9, radio.mac.begin());
            radio.version_major = r[9];
            radio.board = r[10];
            radio.receivers = r[19];
            radio.version_minor = r[21];
            radio.busy = r[2] == 0x03;
            return radio;
        }
        return std::nullopt;
    }

    void start(const StreamConfig& config) override {
        if (config.receivers < 1 || config.receivers > 7) {
            throw std::runtime_error("Protocol 1 takes 1 to 7 receivers");
        }
        if (config.lna_db && (*config.lna_db < -12 || *config.lna_db > 48)) {
            throw std::runtime_error("LNA gain outside -12 to 48 dB");
        }
        config_ = config;
        commands_.clear();
        commands_.push_back(
            {0x00, rate_code(config.rate_ksps) << 24 | (config.receivers - 1) << 3 | 1u << 2});
        commands_.push_back({0x01, config.frequency_hz});
        for (unsigned n = 0; n < config.receivers; ++n) {
            commands_.push_back({static_cast<std::uint8_t>(0x02 + n), config.frequency_hz});
        }
        commands_.push_back({0x09, 0});  // drive 0, PA disabled
        if (config.lna_db) {
            commands_.push_back({0x0a, 0x40u | static_cast<std::uint32_t>(*config.lna_db + 12)});
        }
        samples_per_half_ = kHalfPayload / (6 * config.receivers + 2);
        counts_.levels.assign(config.receivers, Levels{});
        // The whole configuration goes out before the start.
        for (std::size_t i = 0; i < (commands_.size() + 1) / 2; ++i) {
            send_host_packet();
        }
        std::vector<std::uint8_t> start(64, 0);
        start[0] = 0xef;
        start[1] = 0xfe;
        start[2] = 0x04;
        start[3] = 0x01;
        transport_.send(0, start);
        started_ = true;
        rx_samples_ = 0;
        host_packets_after_start_ = 0;
    }

    void set_mox(bool mox) override { mox_ = mox; }

    bool poll(std::chrono::milliseconds timeout) override {
        // Bounded as a whole: stray datagrams must not keep it waiting.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            auto d = transport_.receive(std::max(left, std::chrono::milliseconds(0)));
            if (!d) {
                return false;
            }
            const auto& p = d->bytes;
            if (d->offset != 0 || p.size() != kPacketBytes || p[0] != 0xef || p[1] != 0xfe ||
                p[2] != 0x01 || p[3] != 0x06) {
                continue;
            }
            take(p);
            // Host packets keep pace with the radio's clock: 126 TX samples
            // at 48 ksps each.
            const auto owed = static_cast<std::uint64_t>(
                static_cast<double>(rx_samples_) * kTxRateHz / rate_hz() / kHostSamplesPerPacket);
            while (host_packets_after_start_ < owed) {
                send_host_packet();
                ++host_packets_after_start_;
            }
            return true;
        }
    }

    void stop() override {
        mox_ = false;
        send_host_packet();
        std::vector<std::uint8_t> stop(64, 0);
        stop[0] = 0xef;
        stop[1] = 0xfe;
        stop[2] = 0x04;
        transport_.send(0, stop);
        started_ = false;
    }

    double expected_packets_per_second() const override {
        return rate_hz() / static_cast<double>(2 * samples_per_half_);
    }

private:
    double rate_hz() const { return config_.rate_ksps * 1000.0; }

    void take(const std::vector<std::uint8_t>& p) {
        count_sequence(0, be32(p, 4));
        ++counts_.packets;
        const std::size_t sample_bytes = 6 * config_.receivers + 2;
        for (std::size_t half = 0; half < 2; ++half) {
            const std::size_t at = 8 + half * kHalfBytes;
            if ((p[at + 3] & 0x01) != 0) {
                ++counts_.ptt_reported;
            }
            for (std::size_t s = 0; s < samples_per_half_; ++s) {
                const std::size_t sample = at + 8 + s * sample_bytes;
                for (unsigned n = 0; n < config_.receivers; ++n) {
                    counts_.levels[n].add(s24(p, sample + std::size_t{6} * n),
                                          s24(p, sample + std::size_t{6} * n + 3));
                }
            }
        }
        rx_samples_ += 2 * samples_per_half_;
    }

    // Two command blocks, cycling through the configuration, MOX in C0
    // bit 0 of both, and zero TX samples.
    void send_host_packet() {
        std::vector<std::uint8_t> p(kPacketBytes, 0);
        p[0] = 0xef;
        p[1] = 0xfe;
        p[2] = 0x01;
        p[3] = 0x02;
        put_be32(p, 4, host_sequence_++);
        for (std::size_t half = 0; half < 2; ++half) {
            const std::size_t at = 8 + half * kHalfBytes;
            p[at] = p[at + 1] = p[at + 2] = 0x7f;
            const CommandWord& c = commands_[next_command_++ % commands_.size()];
            p[at + 3] = static_cast<std::uint8_t>(c.address << 1 | (mox_ ? 1 : 0));
            put_be32(p, at + 4, c.data);
        }
        transport_.send(0, p);
        ++counts_.tx_packets;
    }

    Transport& transport_;
    StreamConfig config_;
    std::vector<CommandWord> commands_;
    std::size_t next_command_ = 0;
    std::size_t samples_per_half_ = 63;
    std::uint32_t host_sequence_ = 0;
    std::uint64_t rx_samples_ = 0;
    std::uint64_t host_packets_after_start_ = 0;
    bool mox_ = false;
    bool started_ = false;
};

}  // namespace

std::unique_ptr<Client> make_p1_client(Transport& transport) {
    return std::make_unique<P1Client>(transport);
}

}  // namespace radioberry::proto
