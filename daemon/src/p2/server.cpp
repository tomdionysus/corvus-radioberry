// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2/server.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "device/capacity.hpp"
#include "frequency.hpp"

namespace radioberry::daemon::p2 {
namespace {

constexpr std::uint8_t kLnaGain = 0x0a;
constexpr std::uint32_t kLnaGainMode = 0x40;  // bits 5..0 are the gain + 12 dB
constexpr std::uint32_t kPaEnable = 1u << 19;
constexpr std::uint32_t kDuplex = 1u << 2;
constexpr int kMinGainDb = -12;
constexpr int kMaxGainDb = 48;
constexpr float kFullScale24 = 8388608.0f;

// The Hermes-Lite 2 rate code for a DDC rate, if the gateware has it.
std::optional<unsigned> rate_code(std::uint16_t ksps) {
    switch (ksps) {
        case 48:
            return 0;
        case 96:
            return 1;
        case 192:
            return 2;
        case 384:
            return 3;
        default:
            return std::nullopt;
    }
}

std::int32_t get24(const std::uint8_t* p) {
    const std::uint32_t u =
        static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
    return static_cast<std::int32_t>(u << 8) >> 8;
}

void put24(std::uint8_t* p, std::int32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 16);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v);
}

std::int32_t quantise(float v, float scale, std::int32_t max) {
    const long q = std::lround(v * scale);
    return static_cast<std::int32_t>(std::clamp<long>(q, -max - 1, max));
}

}  // namespace

// One DDC's stream: its decimation from the device rate and the packet
// being filled.
struct Server::DdcStream {
    bool enabled = false;
    unsigned rate_code = 0;
    std::unique_ptr<dsp::Decimator> decimator = std::make_unique<dsp::Decimator>(1);
    LongPacket packet{};
    std::size_t fill = 0;  // samples in `packet`
    std::uint32_t sequence = 0;

    void restart() {
        fill = 0;
        sequence = 0;
        decimator->reset();
    }
};

Server::Server(Identity identity, Control& control, SpscRing<TxBlock>& tx, Session& session,
               TxPolicy& policy, std::function<p1::GatewareStatus()> status, Link& link)
    : identity_(identity),
      control_(control),
      tx_(tx),
      session_(session),
      policy_(policy),
      status_(std::move(status)),
      link_(link),
      tx_decimator_(kTxRateHz / 48000) {
    for (unsigned n = 0; n < identity_.receivers; ++n) {
        ddcs_.push_back(std::make_unique<DdcStream>());
    }
}

Server::~Server() = default;

void Server::on_packet(unsigned port, std::span<const std::uint8_t> packet, const Peer& from,
                       std::chrono::nanoseconds now) {
    std::unique_lock lock(mutex_);
    ++counters_.packets_in;
    const Owner me = owner(from);
    const bool owned = session_.owns(me) && client_ == from;
    switch (port) {
        case kGeneralPort:
            if (is_p2_discovery(packet)) {
                const ShortPacket reply = discovery_reply(identity_, session_.owner().has_value());
                lock.unlock();
                spdlog::info("Protocol 2 discovery from {}", describe(from));
                link_.send(kGeneralPort, from, reply);
                return;
            }
            if (const auto g = parse_general(packet)) {
                if (!claim_locked(from, now)) {
                    break;
                }
                check_sequence_locked(last_general_, g->sequence);
                general_locked(*g);
                return;
            }
            break;
        // Receiver and Transmitter Specific arrive on other sockets, read by
        // another thread, so they can overtake the client's General: while
        // the radio is free they claim it too, or the first configuration
        // would be lost.
        case kRxSpecificPort:
            if (const auto r = parse_rx_specific(packet); r && (owned || claim_locked(from, now))) {
                session_.heard(me, now);
                check_sequence_locked(last_rx_specific_, r->sequence);
                rx_specific_locked(*r);
                return;
            }
            break;
        case kTxSpecificPort:
            if (const auto t = parse_tx_specific(packet); t && (owned || claim_locked(from, now))) {
                session_.heard(me, now);
                check_sequence_locked(last_tx_specific_, t->sequence);
                tx_specific_locked(*t);
                return;
            }
            break;
        case kHighPriorityPort:
            if (const auto h = parse_high_priority(packet)) {
                if (h->run) {
                    if (!claim_locked(from, now)) {
                        break;
                    }
                } else if (!owned) {
                    break;
                }
                session_.heard(me, now);
                check_sequence_locked(last_high_priority_, h->sequence);
                if (!h->run) {
                    running_ = false;
                    lock.unlock();
                    session_.release(me, "the client stopped it");
                    return;
                }
                if (!running_) {
                    start_locked();
                }
                high_priority_locked(*h, now);
                return;
            }
            break;
        case kAudioPort:
            if (owned) {
                session_.heard(me, now);  // speaker audio: there is no speaker
                return;
            }
            break;
        case kTxIqPort:
            if (const auto t = parse_tx_iq(packet); t && owned && running_) {
                session_.heard(me, now);
                tx_iq_locked(*t);
                return;
            }
            break;
        default:
            break;
    }
    ++counters_.ignored;
}

void Server::on_rx(const RxBlock& block) {
    const p1::GatewareStatus status = status_();
    std::size_t n = 0;
    Peer to;
    {
        const std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        to = client_;
        const std::size_t frames = block.frames();
        const std::uint32_t rate = rx_rate_hz(block.rate_code);

        // Mic/Line: 48 kHz of silence by the radio's clock.
        mic_accumulator_ += static_cast<std::uint64_t>(frames) * kMicRateHz;
        const std::uint64_t per_packet = static_cast<std::uint64_t>(rate) * kMicSamplesPerPacket;
        while (mic_accumulator_ >= per_packet) {
            mic_accumulator_ -= per_packet;
            const MicPacket m = mic_packet(mic_sequence_++);
            Outgoing& o = outgoing_locked(n);
            o.port = kMicLinePort;
            o.size = m.size();
            std::copy(m.begin(), m.end(), o.bytes.begin());
        }

        // High Priority status: every 50 ms, and at once on a change.
        status_frames_ += frames;
        const std::pair<bool, bool> inputs{status.cw_or_ptt, status.cw_key};
        if (status_sent_ != inputs || status_frames_ >= rate / 20) {
            status_frames_ = 0;
            status_sent_ = inputs;
            const ShortPacket s =
                high_priority_status(status_sequence_++, inputs.first, inputs.second);
            Outgoing& o = outgoing_locked(n);
            o.port = kHighPriorityStatusPort;
            o.size = s.size();
            std::copy(s.begin(), s.end(), o.bytes.begin());
        }

        // DDCs, once the device runs the rate asked for.
        if (block.rate_code == device_rate_code_) {
            const std::size_t frame_bytes = rx_frame_bytes(block.receivers);
            for (std::size_t d = 0; d < ddcs_.size(); ++d) {
                DdcStream& s = *ddcs_[d];
                if (!s.enabled) {
                    continue;
                }
                const bool live = d < block.receivers;
                for (std::size_t f = 0; f < frames; ++f) {
                    const std::uint8_t* in =
                        live ? block.bytes.data() + f * frame_bytes + d * kSampleBytes : nullptr;
                    std::int32_t i = 0;
                    std::int32_t q = 0;
                    if (s.decimator->factor() == 1) {
                        if (live) {
                            i = get24(in);
                            q = get24(in + 3);
                        }
                    } else {
                        const std::complex<float> x =
                            live ? std::complex<float>(
                                       static_cast<float>(get24(in)) / kFullScale24,
                                       static_cast<float>(get24(in + 3)) / kFullScale24)
                                 : std::complex<float>();
                        std::complex<float> y;
                        if (!s.decimator->push(x, y)) {
                            continue;
                        }
                        i = quantise(y.real(), kFullScale24, 8388607);
                        q = quantise(y.imag(), kFullScale24, 8388607);
                    }
                    if (s.fill == 0) {
                        begin_ddc_packet(s.packet, s.sequence);
                    }
                    std::uint8_t* out = s.packet.data() + kDdcHeaderBytes + s.fill * kSampleBytes;
                    put24(out, swap_iq_ ? q : i);
                    put24(out + 3, swap_iq_ ? i : q);
                    if (++s.fill == kDdcSamplesPerPacket) {
                        Outgoing& o = outgoing_locked(n);
                        o.port = kDdcPort0 + static_cast<unsigned>(d);
                        o.size = s.packet.size();
                        o.bytes = s.packet;
                        s.fill = 0;
                        ++s.sequence;
                        ++counters_.ddc_packets_out;
                    }
                }
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        link_.send(outgoing_[i].port, to,
                   std::span<const std::uint8_t>(outgoing_[i].bytes.data(), outgoing_[i].size));
    }
}

void Server::tick() {
    const std::lock_guard lock(mutex_);
    if (running_ && !session_.owns(owner(client_))) {
        running_ = false;
    }
}

void Server::set_frequency_ppm(double ppm) {
    const std::lock_guard lock(mutex_);
    frequency_ppm_ = ppm;
}

void Server::set_iq_convention(p1::IqConvention convention) {
    const std::lock_guard lock(mutex_);
    swap_iq_ = convention == p1::IqConvention::Standard;
}

void Server::set_rx_gain_db(int db) {
    const std::lock_guard lock(mutex_);
    rx_gain_db_ = std::clamp(db, kMinGainDb, kMaxGainDb);
}

bool Server::running() const {
    const std::lock_guard lock(mutex_);
    return running_;
}

std::optional<Peer> Server::client() const {
    const std::lock_guard lock(mutex_);
    return running_ ? std::optional<Peer>(client_) : std::nullopt;
}

unsigned Server::device_receivers() const {
    const std::lock_guard lock(mutex_);
    return device_receivers_;
}

unsigned Server::device_rate_code() const {
    const std::lock_guard lock(mutex_);
    return device_rate_code_;
}

ServerCounters Server::counters() const {
    const std::lock_guard lock(mutex_);
    return counters_;
}

bool Server::claim_locked(const Peer& from, std::chrono::nanoseconds now) {
    const Owner me = owner(from);
    if (session_.busy_for(me) || !session_.claim(me, now)) {
        return false;
    }
    if (!(client_ == from)) {
        running_ = false;
        client_ = from;
        last_general_.reset();
        last_rx_specific_.reset();
        last_tx_specific_.reset();
        last_high_priority_.reset();
    }
    return true;
}

void Server::start_locked() {
    running_ = true;
    for (const auto& s : ddcs_) {
        s->restart();
    }
    tx_decimator_.reset();
    last_tx_iq_.reset();
    mic_sequence_ = 0;
    mic_accumulator_ = 0;
    status_sequence_ = 0;
    status_frames_ = 0;
    status_sent_.reset();
    spdlog::info("Protocol 2 stream to {}", describe(client_));
}

void Server::general_locked(const General& g) {
    phase_words_ = g.phase_words;
    if (g.pa_enable != pa_enable_) {
        pa_enable_ = g.pa_enable;
        drive_locked();
    }
    if (!g.default_ports && !warned_ports_) {
        spdlog::warn("Protocol 2 client asked for other ports: not supported, the defaults stay");
        warned_ports_ = true;
    }
    if (g.wideband && !warned_wideband_) {
        spdlog::warn("Protocol 2 wideband data asked for: this radio has none");
        warned_wideband_ = true;
    }
}

void Server::rx_specific_locked(const RxSpecific& r) {
    std::vector<std::optional<unsigned>> wanted(ddcs_.size());
    std::optional<unsigned> top;
    std::size_t highest = 0;
    for (unsigned n = 0; n < kMaxDdcs; ++n) {
        const Ddc& d = r.ddc[n];
        if (!d.enabled) {
            continue;
        }
        const auto code = rate_code(d.rate_ksps);
        if (n >= ddcs_.size() || !code) {
            spdlog::debug("DDC{} at {} ksps not served", n, d.rate_ksps);
            continue;
        }
        if (d.sync != 0 && !warned_sync_) {
            spdlog::warn("synchronised DDCs (diversity, PureSignal) are not supported");
            warned_sync_ = true;
        }
        if (d.adc != 0 && !warned_adc_) {
            spdlog::warn("DDC{} asked for ADC{}: this radio has one ADC, every DDC uses it", n,
                         d.adc);
            warned_adc_ = true;
        }
        wanted[n] = code;
        top = top ? std::max(*top, *code) : *code;
        highest = n;
    }
    if (!top) {
        // Nothing enabled, as while transmitting without duplex: the
        // device keeps its layout, so the radio's clock keeps pacing.
        for (const auto& s : ddcs_) {
            s->enabled = false;
        }
        return;
    }
    const unsigned want = static_cast<unsigned>(highest) + 1;
    const unsigned run =
        std::min(want, max_receivers(*top, identity_.receivers, identity_.rx_values_max));
    if (*top != device_rate_code_ || run != device_receivers_) {
        if (run < want) {
            spdlog::warn(
                "DDC0 to DDC{} at up to {} ksps asked for; the gateware runs {}, the rest "
                "are sent as zero",
                highest, rx_rate_hz(*top) / 1000, run);
        } else {
            spdlog::info("DDC0 to DDC{} at up to {} ksps", highest, rx_rate_hz(*top) / 1000);
        }
        device_rate_code_ = *top;
        device_receivers_ = run;
        command_locked(Control::kGeneral, *top << 24 | (run - 1) << 3 | kDuplex);
    }
    for (std::size_t n = 0; n < ddcs_.size(); ++n) {
        DdcStream& s = *ddcs_[n];
        const std::optional<unsigned> want_code = wanted[n];
        if (!want_code) {
            s.enabled = false;
            continue;
        }
        const unsigned factor = 1u << (*top - *want_code);
        if (!s.enabled || s.rate_code != *want_code || s.decimator->factor() != factor) {
            s.rate_code = *want_code;
            s.decimator = std::make_unique<dsp::Decimator>(factor);
            s.fill = 0;
        }
        s.enabled = true;
    }
}

void Server::tx_specific_locked(const TxSpecific& t) {
    if (((t.rate_ksps != 0 && t.rate_ksps != kTxRateHz / 1000) || (t.bits != 0 && t.bits != 24)) &&
        !warned_tx_format_) {
        spdlog::warn("TX I/Q at {} ksps, {} bits asked for: taken as 192 ksps, 24 bits",
                     t.rate_ksps, t.bits);
        warned_tx_format_ = true;
    }
    if ((t.cw_mode & 0x02) != 0 && !warned_cw_) {
        spdlog::warn("CW keyed in the radio asked for: not supported, key CW from the client");
        warned_cw_ = true;
    }
}

void Server::high_priority_locked(const HighPriority& h, std::chrono::nanoseconds now) {
    const auto hz = [&](std::uint32_t f) {
        return corrected_frequency(phase_words_ ? frequency_from_phase_word(f) : f, frequency_ppm_);
    };
    for (std::size_t n = 0; n < ddcs_.size(); ++n) {
        if (h.ddc_frequency[n] != 0) {
            command_locked(static_cast<std::uint8_t>(0x02 + n), hz(h.ddc_frequency[n]));
        }
    }
    if (h.duc_frequency != 0) {
        command_locked(0x01, hz(h.duc_frequency));
    }
    attenuation_ = h.attenuation_adc0;
    gain_locked();
    drive_ = h.drive;
    drive_locked();
    policy_.set_cwx(h.cwx);
    policy_.update(owner(client_), h.mox, now);
}

void Server::tx_iq_locked(const TxIq& t) {
    if (last_tx_iq_ && t.sequence != *last_tx_iq_ + 1) {
        ++counters_.tx_iq_sequence_gaps;
        session_.fault("Protocol 2 TX I/Q sequence gap");
    }
    last_tx_iq_ = t.sequence;
    // Only samples sent while MOX is granted go to the device (as Protocol
    // 1). The decimator still takes every sample, so its filter holds the
    // client's signal when transmission starts.
    TxBlock* block = nullptr;
    if (policy_.transmitting()) {
        block = tx_.acquire();
        if (block == nullptr) {
            ++counters_.tx_blocks_dropped;
            session_.fault("TX queue full");
            return;
        }
    }
    std::size_t out = 0;
    for (std::size_t k = 0; k < kTxSamplesPerPacket; ++k) {
        const std::uint8_t* in = t.samples.data() + k * kSampleBytes;
        const std::complex<float> x(static_cast<float>(get24(in)) / kFullScale24,
                                    static_cast<float>(get24(in + 3)) / kFullScale24);
        std::complex<float> y;
        if (!tx_decimator_.push(x, y) || block == nullptr) {
            continue;
        }
        const std::int32_t i = quantise(y.real(), 32768.0f, 32767);
        const std::int32_t q = quantise(y.imag(), 32768.0f, 32767);
        const std::int32_t first = swap_iq_ ? q : i;
        const std::int32_t second = swap_iq_ ? i : q;
        std::uint8_t* p = block->bytes.data() + out * kTxSampleBytes;
        p[0] = static_cast<std::uint8_t>(first >> 8);
        p[1] = static_cast<std::uint8_t>(first);
        p[2] = static_cast<std::uint8_t>(second >> 8);
        p[3] = static_cast<std::uint8_t>(second);
        ++out;
    }
    if (block != nullptr) {
        block->size = out * kTxSampleBytes;
        tx_.publish();
    }
}

void Server::command_locked(std::uint8_t address, std::uint32_t data) {
    if (control_.value(address) == data) {
        return;
    }
    Command command;
    command.address = address;
    command.data = data;
    try {
        control_.set(command);
        spdlog::debug("command 0x{:02x} data 0x{:08x}", address, data);
    } catch (const std::invalid_argument& e) {
        spdlog::warn("command 0x{:02x} data 0x{:08x} refused: {}", address, data, e.what());
    }
}

void Server::drive_locked() {
    policy_.set_tx_drive(static_cast<std::uint32_t>(drive_) << 24 | (pa_enable_ ? kPaEnable : 0u));
}

void Server::gain_locked() {
    const int db = std::clamp(rx_gain_db_ - attenuation_, kMinGainDb, kMaxGainDb);
    command_locked(kLnaGain, kLnaGainMode | static_cast<std::uint32_t>(db - kMinGainDb));
}

void Server::check_sequence_locked(std::optional<std::uint32_t>& last, std::uint32_t sequence) {
    if (last && sequence != *last + 1) {
        ++counters_.command_sequence_gaps;
    }
    last = sequence;
}

Server::Outgoing& Server::outgoing_locked(std::size_t& n) {
    if (outgoing_.size() <= n) {
        outgoing_.emplace_back();
    }
    return outgoing_[n++];
}

}  // namespace radioberry::daemon::p2
