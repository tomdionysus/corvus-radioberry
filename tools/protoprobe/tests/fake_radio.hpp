// SPDX-License-Identifier: GPL-2.0-or-later
//
// A scripted radio behind the Transport interface, for Protocol 1 or 2,
// with its own clock: each stream packet it hands out advances the clock to
// the time the radio would have sent it, so rates come out exact.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

#include "client.hpp"

namespace radioberry::proto::test {

class FakeRadio final : public Transport {
public:
    explicit FakeRadio(bool protocol_two) : two_(protocol_two) {}

    Clock clock() {
        return [this] { return now; };
    }

    void send(unsigned offset, std::span<const std::uint8_t> b) override {
        if (fail_sends) {
            throw std::runtime_error("send failed");
        }
        sent.push_back({offset, std::vector<std::uint8_t>(b.begin(), b.end())});
        if (two_) {
            take_p2(offset, b);
        } else {
            take_p1(b);
        }
    }

    std::optional<Datagram> receive(std::chrono::milliseconds timeout) override {
        if (!queued.empty()) {
            Datagram d = queued.front();
            queued.erase(queued.begin());
            return d;
        }
        if (!streaming || mute) {
            now += timeout;
            return std::nullopt;
        }
        return two_ ? next_p2() : next_p1();
    }

    // Script.
    bool answer = true;
    bool busy = false;
    unsigned receivers = 6;
    double rate_scale = 1.0;       // stream faster or slower than configured
    std::uint32_t skip_every = 0;  // drop one stream packet in this many
    bool report_ptt = false;       // echo MOX as PTT in the status
    bool mute = false;             // answers discovery, never streams
    bool fail_sends = false;       // send() throws
    std::int32_t sample = 1 << 20;

    // Observed.
    std::chrono::nanoseconds now{1'000'000'000};
    std::vector<Datagram> sent;
    std::vector<Datagram> queued;
    bool streaming = false;
    bool mox = false;
    bool mox_seen = false;
    unsigned rate_ksps = 48;
    unsigned active = 1;
    std::map<unsigned, std::uint32_t> p1_commands;  // address -> data
    std::uint64_t tx_iq_packets = 0;
    std::uint32_t tx_iq_last = 0;
    bool tx_iq_gap = false;
    std::uint64_t host_packets = 0;

private:
    static void put32(std::vector<std::uint8_t>& p, std::size_t at, std::uint32_t v) {
        put_be32(p, at, v);
    }
    static void put24(std::vector<std::uint8_t>& p, std::size_t at, std::int32_t v) {
        p[at] = static_cast<std::uint8_t>(v >> 16);
        p[at + 1] = static_cast<std::uint8_t>(v >> 8);
        p[at + 2] = static_cast<std::uint8_t>(v);
    }

    std::uint32_t next_sequence(std::uint32_t& seq) {
        ++emitted_;
        if (skip_every != 0 && emitted_ % skip_every == 0) {
            ++seq;
        }
        return seq++;
    }

    // Protocol 1.
    void take_p1(std::span<const std::uint8_t> b) {
        if (b.size() >= 3 && b[0] == 0xef && b[1] == 0xfe && b[2] == 0x02) {
            if (answer) {
                std::vector<std::uint8_t> r(60, 0);
                r[0] = 0xef;
                r[1] = 0xfe;
                r[2] = busy ? 0x03 : 0x02;
                r[3] = 2;
                r[8] = 9;
                r[9] = 75;
                r[10] = 6;
                r[19] = static_cast<std::uint8_t>(receivers);
                r[21] = 3;
                queued.push_back({0, r});
            }
        } else if (b.size() == 64 && b[2] == 0x04) {
            streaming = b[3] == 0x01;
            next_ = now;
        } else if (b.size() == 1032 && b[2] == 0x01) {
            ++host_packets;
            for (std::size_t half = 0; half < 2; ++half) {
                const std::size_t at = 8 + half * 512;
                const unsigned address = b[at + 3] >> 1;
                mox = (b[at + 3] & 1) != 0;
                mox_seen = mox_seen || mox;
                p1_commands[address] = be32(b, at + 4);
                if (address == 0) {
                    rate_ksps = 48u << (p1_commands[0] >> 24 & 3);
                    active = (p1_commands[0] >> 3 & 7) + 1;
                }
            }
        }
    }

    Datagram next_p1() {
        const std::size_t per_half = 504 / (6 * active + 2);
        const double period = static_cast<double>(2 * per_half) / (rate_ksps * 1000.0) / rate_scale;
        now = next_;
        next_ += std::chrono::nanoseconds(static_cast<long long>(period * 1e9));
        std::vector<std::uint8_t> p(1032, 0);
        p[0] = 0xef;
        p[1] = 0xfe;
        p[2] = 0x01;
        p[3] = 0x06;
        put32(p, 4, next_sequence(sequence_));
        for (std::size_t half = 0; half < 2; ++half) {
            const std::size_t at = 8 + half * 512;
            p[at] = p[at + 1] = p[at + 2] = 0x7f;
            p[at + 3] = (report_ptt && mox) ? 0x01 : 0x00;
            for (std::size_t s = 0; s < per_half; ++s) {
                for (unsigned n = 0; n < active; ++n) {
                    const std::size_t x = at + 8 + s * (6 * active + 2) + 6 * n;
                    put24(p, x, sample);
                    put24(p, x + 3, -sample);
                }
            }
        }
        return {0, p};
    }

    // Protocol 2.
    void take_p2(unsigned offset, std::span<const std::uint8_t> b) {
        if (offset == 0 && b.size() == 60 && b[4] == 0x02) {
            if (answer) {
                std::vector<std::uint8_t> r(60, 0);
                r[4] = busy ? 0x03 : 0x02;
                r[10] = 9;
                r[11] = 6;
                r[12] = 38;
                r[13] = 75;
                r[20] = static_cast<std::uint8_t>(receivers);
                queued.push_back({0, r});
            }
        } else if (offset == 1 && b.size() == 1444) {
            active = 0;
            for (unsigned n = 0; n < 8; ++n) {
                if ((b[7] >> n & 1) != 0) {
                    active = n + 1;
                    rate_ksps = static_cast<unsigned>(b[18 + 6 * n] << 8 | b[19 + 6 * n]);
                }
            }
        } else if (offset == 3 && b.size() == 1444) {
            const bool run = (b[4] & 1) != 0;
            if (run && !streaming) {
                next_ = now;
                next_mic_ = now;
            }
            streaming = run;
            mox = (b[4] & 2) != 0;
            mox_seen = mox_seen || mox;
        } else if (offset == 5 && b.size() == 1444) {
            const std::uint32_t seq = be32(b, 0);
            if (tx_iq_packets > 0 && seq != tx_iq_last + 1) {
                tx_iq_gap = true;
            }
            tx_iq_last = seq;
            ++tx_iq_packets;
        }
    }

    Datagram next_p2() {
        // DDC packets round-robin across the DDCs, Mic/Line packets every 64
        // samples at 48 ksps, and a status packet with each Mic/Line packet.
        if (pending_status_) {
            pending_status_ = false;
            std::vector<std::uint8_t> s(60, 0);
            s[4] = (report_ptt && mox) ? 0x01 : 0x00;
            return {1, s};
        }
        if (next_mic_ <= next_) {
            now = next_mic_;
            next_mic_ +=
                std::chrono::nanoseconds(static_cast<long long>(64.0 / 48000.0 / rate_scale * 1e9));
            std::vector<std::uint8_t> m(132, 0);
            put32(m, 0, mic_sequence_++);
            pending_status_ = true;
            return {2, m};
        }
        now = next_;
        const unsigned ddc = ddc_turn_;
        ddc_turn_ = (ddc_turn_ + 1) % active;
        if (ddc_turn_ == 0) {
            next_ += std::chrono::nanoseconds(
                static_cast<long long>(238.0 / (rate_ksps * 1000.0) / rate_scale * 1e9));
        }
        std::vector<std::uint8_t> p(1444, 0);
        put32(p, 0, next_sequence(ddc_sequence_[ddc]));
        p[13] = 24;
        p[14] = 0;
        p[15] = 238;
        for (std::size_t s = 0; s < 238; ++s) {
            put24(p, 16 + 6 * s, sample);
            put24(p, 19 + 6 * s, -sample);
        }
        return {11 + ddc, p};
    }

    bool two_;
    std::chrono::nanoseconds next_{};
    std::chrono::nanoseconds next_mic_{};
    std::uint32_t sequence_ = 0;
    std::uint32_t mic_sequence_ = 0;
    std::map<unsigned, std::uint32_t> ddc_sequence_;
    unsigned ddc_turn_ = 0;
    bool pending_status_ = false;
    std::uint64_t emitted_ = 0;
};

}  // namespace radioberry::proto::test
