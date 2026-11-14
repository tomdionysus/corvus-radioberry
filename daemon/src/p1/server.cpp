// SPDX-License-Identifier: GPL-2.0-or-later
#include "p1/server.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "device/capacity.hpp"
#include "frequency.hpp"

namespace radioberry::daemon::p1 {

Server::Server(Identity identity, Control& control, SpscRing<TxBlock>& tx, Session& session,
               TxPolicy& policy, std::function<GatewareStatus()> status, Link& link)
    : identity_(identity),
      control_(control),
      tx_(tx),
      session_(session),
      policy_(policy),
      status_(std::move(status)),
      link_(link) {}

void Server::on_packet(std::span<const std::uint8_t> packet, const Peer& from,
                       std::chrono::nanoseconds now) {
    std::unique_lock lock(mutex_);
    ++counters_.packets_in;
    switch (classify(packet)) {
        case PacketKind::Discovery: {
            const auto reply = discovery_reply(identity_, running_);
            const bool streaming_peer = running_ && client_ == from;
            lock.unlock();
            spdlog::info("discovery from {}", describe(from));
            link_.send(from, reply);
            if (from.tcp && !streaming_peer) {
                link_.close(from);
            }
            return;
        }
        case PacketKind::StartStop: {
            if (session_.busy_for(owner(from))) {
                ++counters_.ignored;
                return;
            }
            // classify() found a start/stop packet, which always parses.
            if (parse_start(packet).value_or(false)) {
                if (session_.claim(owner(from), now)) {
                    start_locked(from);
                }
                return;
            }
            running_ = false;
            lock.unlock();
            session_.release(owner(from), "the client stopped it");
            if (from.tcp) {
                link_.close(from);
            }
            return;
        }
        case PacketKind::Data: {
            const auto host = parse_host_packet(packet);
            if (!host || session_.busy_for(owner(from))) {
                ++counters_.ignored;
                return;
            }
            const bool from_client = running_ && client_ == from;
            if (from_client) {
                session_.heard(owner(from), now);
                if (last_host_sequence_ && host->sequence != *last_host_sequence_ + 1 &&
                    host->sequence != 0) {
                    ++counters_.host_sequence_gaps;
                    session_.fault("Protocol 1 host packet sequence gap");
                }
                last_host_sequence_ = host->sequence;
            }
            for (const HostCommand& c : host->commands) {
                apply_locked(c);
            }
            if (from_client) {
                // The second block is the later command; its MOX stands.
                policy_.update(owner(from), host->commands[1].mox, now);
                queue_tx_locked(*host);
            }
            return;
        }
        case PacketKind::Unknown:
            ++counters_.ignored;
            return;
    }
}

void Server::on_rx(const RxBlock& block) {
    const GatewareStatus status = status_();
    std::size_t n = 0;
    Peer to;
    {
        const std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        to = client_;
        packer_.add(
            block,
            [&] {
                if (pending_ack_) {
                    const Word w =
                        ack_word(pending_ack_->address, status.cw_or_ptt, pending_ack_->data);
                    pending_ack_.reset();
                    return w;
                }
                return status_word(status_address_++ & 3u, status.cw_or_ptt, status.cw_key,
                                   identity_.version_major);
            },
            [&](std::span<const std::uint8_t> p) {
                if (outgoing_.size() <= n) {
                    outgoing_.emplace_back();
                }
                std::copy(p.begin(), p.end(), outgoing_[n].begin());
                ++n;
            });
        counters_.packets_out += n;
    }
    for (std::size_t i = 0; i < n; ++i) {
        link_.send(to, outgoing_[i]);
    }
}

void Server::tick() {
    std::unique_lock lock(mutex_);
    if (!running_ || session_.owns(owner(client_))) {
        return;
    }
    running_ = false;
    const Peer gone = client_;
    lock.unlock();
    if (gone.tcp) {
        link_.close(gone);
    }
}

void Server::set_iq_convention(IqConvention convention) {
    const std::lock_guard lock(mutex_);
    swap_iq_ = convention == IqConvention::Standard;
    packer_.set_swap_iq(swap_iq_);
}

void Server::set_frequency_ppm(double ppm) {
    const std::lock_guard lock(mutex_);
    frequency_ppm_ = ppm;
}

bool Server::running() const {
    const std::lock_guard lock(mutex_);
    return running_;
}

std::optional<Peer> Server::client() const {
    const std::lock_guard lock(mutex_);
    return running_ ? std::optional<Peer>(client_) : std::nullopt;
}

unsigned Server::client_receivers() const {
    const std::lock_guard lock(mutex_);
    return client_receivers_;
}

unsigned Server::device_receivers() const {
    const std::lock_guard lock(mutex_);
    return device_receivers_;
}

ServerCounters Server::counters() const {
    const std::lock_guard lock(mutex_);
    return counters_;
}

void Server::start_locked(const Peer& from) {
    running_ = true;
    client_ = from;
    last_host_sequence_.reset();
    packer_.reset();
    status_address_ = 0;
    pending_ack_.reset();
}

void Server::apply_locked(const HostCommand& c) {
    if (c.ack) {
        pending_ack_ = c;
    }
    std::uint32_t data = c.data;
    if (c.address == Control::kGeneral) {
        const unsigned want = ((data >> 3) & 0x0fu) + 1;
        const unsigned rate = (data >> 24) & 0x03u;
        const unsigned run =
            std::min(want, max_receivers(rate, identity_.receivers, identity_.rx_values_max));
        if (want != client_receivers_ || run != device_receivers_) {
            if (run < want) {
                spdlog::warn("{} receivers at {} ksps asked for; running {}, the rest sent as zero",
                             want, rx_rate_hz(rate) / 1000, run);
            } else {
                spdlog::info("{} receivers at {} ksps", want, rx_rate_hz(rate) / 1000);
            }
        }
        client_receivers_ = want;
        device_receivers_ = run;
        packer_.set_receivers(want);
        data = (data & ~0x78u) | (run - 1) << 3;
    } else if (c.address == Control::kTxDrive) {
        policy_.set_tx_drive(data);
        return;
    } else if (c.address == kCwConfig) {
        policy_.set_cwx((data & kCwxEnable) != 0);
    } else if (is_frequency_address(c.address)) {
        data = corrected_frequency(data, frequency_ppm_);
    }
    if (control_.value(c.address) == data || refused_[c.address] == data) {
        return;
    }
    Command command;
    command.address = c.address;
    command.data = data;
    try {
        control_.set(command);
        refused_[c.address].reset();
        spdlog::debug("command 0x{:02x} data 0x{:08x}", c.address, data);
    } catch (const std::invalid_argument& e) {
        refused_[c.address] = data;
        spdlog::warn("command 0x{:02x} data 0x{:08x} refused: {}", c.address, data, e.what());
    }
}

void Server::queue_tx_locked(const HostPacket& packet) {
    // Clients send TX samples between overs too, as zeros. Only samples
    // sent while MOX is granted go to the device; the rest would cost the
    // driver, the link and the gateware work for nothing.
    if (!policy_.transmitting()) {
        return;
    }
    TxBlock* block = tx_.acquire();
    if (block == nullptr) {
        ++counters_.tx_blocks_dropped;
        session_.fault("TX queue full");
        return;
    }
    std::copy(packet.tx_iq.begin(), packet.tx_iq.end(), block->bytes.begin());
    block->size = packet.tx_iq.size();
    if (swap_iq_) {
        swap_tx_iq(std::span<std::uint8_t>(block->bytes.data(), block->size));
    }
    tx_.publish();
}

}  // namespace radioberry::daemon::p1
