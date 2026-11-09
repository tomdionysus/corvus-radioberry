// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "transport.hpp"

namespace radioberry::proto {

using Clock = std::function<std::chrono::nanoseconds()>;

// What discovery reports.
struct Radio {
    std::array<std::uint8_t, 6> mac{};
    unsigned board = 0;
    unsigned version_major = 0;
    std::optional<unsigned> version_minor;     // Protocol 1 only
    std::optional<unsigned> protocol_version;  // Protocol 2 only
    unsigned receivers = 0;
    bool busy = false;  // already streaming to another client
};

struct StreamConfig {
    unsigned rate_ksps = 48;  // 48, 96, 192 or 384
    unsigned receivers = 1;
    std::uint32_t frequency_hz = 7074000;
    std::optional<int> lna_db;  // Protocol 1 only, -12 to 48
};

// RMS and peak of one receiver's 24-bit I/Q, in dB below full scale.
class Levels {
public:
    void add(std::int32_t i, std::int32_t q);
    std::uint64_t samples() const { return samples_; }
    double rms_dbfs() const;
    double peak_dbfs() const;

private:
    std::uint64_t samples_ = 0;
    double sum_squares_ = 0;
    std::int32_t peak_ = 0;
};

// Gaps in one stream's 32-bit sequence numbers.
class Sequence {
public:
    void add(std::uint32_t sequence);
    std::uint64_t breaks() const { return breaks_; }
    std::uint64_t lost() const { return lost_; }
    void reset_counts() { breaks_ = lost_ = 0; }

private:
    std::optional<std::uint32_t> last_;
    std::uint64_t breaks_ = 0;
    std::uint64_t lost_ = 0;
};

struct StreamCounts {
    std::uint64_t packets = 0;  // receive stream packets
    std::uint64_t sequence_breaks = 0;
    std::uint64_t lost_packets = 0;
    std::vector<Levels> levels;  // per receiver
    // Packets in which the radio reported PTT (Protocol 1 C0 bit 0,
    // Protocol 2 High Priority status bit 0).
    std::uint64_t ptt_reported = 0;
    // Transmit-side packets the client sent: Protocol 1 host packets,
    // Protocol 2 TX I/Q packets.
    std::uint64_t tx_packets = 0;
};

// A minimal openHPSDR client. It never sends drive above 0 or PA enable;
// MOX is set only by set_mox(true), with zero TX samples. Failures throw
// std::runtime_error or std::system_error.
class Client {
public:
    virtual ~Client() = default;

    virtual std::optional<Radio> discover(std::chrono::milliseconds timeout) = 0;
    virtual void start(const StreamConfig& config) = 0;
    virtual void set_mox(bool mox) = 0;
    // Handles whatever arrives for up to `timeout`, answering as the
    // protocol needs; returns after the first receive stream packet, or
    // false when none came.
    virtual bool poll(std::chrono::milliseconds timeout) = 0;
    virtual void stop() = 0;
    // Receive stream packets per second the configuration implies.
    virtual double expected_packets_per_second() const = 0;

    const StreamCounts& counts() const { return counts_; }
    // Starts a new measurement; sequence continuity is kept.
    void reset_counts();

protected:
    StreamCounts counts_;
    std::vector<Sequence> sequences_;
    void count_sequence(std::size_t stream, std::uint32_t sequence);
};

std::unique_ptr<Client> make_p1_client(Transport& transport);
std::unique_ptr<Client> make_p2_client(Transport& transport, Clock now);

// Big-endian fields and 24-bit samples.
std::uint32_t be32(std::span<const std::uint8_t> p, std::size_t at);
void put_be32(std::span<std::uint8_t> p, std::size_t at, std::uint32_t v);
std::int32_t s24(std::span<const std::uint8_t> p, std::size_t at);

}  // namespace radioberry::proto
