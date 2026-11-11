// SPDX-License-Identifier: GPL-2.0-or-later
//
// The gateware's side of the RX and TX buses, edge by edge, for testing
// the CPU-clocked bus engine (driver/kernel/rb_bitbang.h). Follows the PIO
// variant's rx_pi_pio.v, tx_pi_pio.v and the registered TX ready in
// radioberry_core.v (gateware/README.md sections 5 and 7):
//
// RX: on each rising edge of the RX clock the state machine advances. In
//     START it moves on only when its FIFO holds more than 12 values (the
//     read side's count, registered on the RX clock), presenting
//     sample-ready high and data 0 meanwhile; otherwise sample-ready is low
//     and the data pins keep their last value. Then the frame index nibble
//     and six data nibbles, one per edge.
// TX: on each rising edge of the TX clock an idle shifter starts if the
//     registered ready is high (that edge carries no data); a running one
//     takes one bit, most significant first, and after 32 stores the word.
//     Ready is then registered: high while the FIFO has more than 8 bytes
//     free.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace radioberry::test {

class GatewareBusSim {
public:
    static constexpr std::uint32_t kTxClk = 1u << 4;
    static constexpr std::uint32_t kTxData = 1u << 5;
    static constexpr std::uint32_t kRxClk = 1u << 6;
    static constexpr std::uint32_t kTxReady = 1u << 12;
    static constexpr unsigned kRxD0 = 18;
    static constexpr std::uint32_t kRxReady = 1u << 25;
    static constexpr std::size_t kRxStartAbove = 12;
    static constexpr std::size_t kTxFifoBytes = 16384;
    static constexpr std::size_t kTxReadyMargin = 8;

    // RX values the gateware will send, one frame of 2 x receivers values
    // at a time; the last of each frame resets the index.
    void push_frame(const std::vector<std::uint32_t>& values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            rx_fifo_.push_back({values[i] & 0xffffff, i + 1 == values.size()});
        }
    }
    // One value; `last` ends its frame.
    void push_value(std::uint32_t value, bool last) {
        rx_fifo_.push_back({value & 0xffffff, last});
    }
    std::size_t rx_queued() const { return rx_fifo_.size(); }

    // TX words the gateware has received, oldest first; draining frees FIFO
    // space as the gateware's reader would.
    const std::vector<std::uint32_t>& tx_received() const { return tx_received_; }
    void tx_drain(std::size_t words) {
        tx_fifo_words_ = words > tx_fifo_words_ ? 0 : tx_fifo_words_ - words;
    }
    std::size_t tx_fifo_words() const { return tx_fifo_words_; }

    // Pi side: GPSET0, GPCLR0, GPLEV0.
    void set(std::uint32_t mask) {
        const std::uint32_t rising = mask & ~out_;
        out_ |= mask;
        if (rising & kRxClk) {
            rx_edge();
        }
        if (rising & kTxClk) {
            tx_edge();
        }
    }
    void clr(std::uint32_t mask) { out_ &= ~mask; }
    std::uint32_t lev() const {
        std::uint32_t v = out_ & (kTxClk | kTxData | kRxClk);
        v |= static_cast<std::uint32_t>(rx_nibble()) << kRxD0;
        if (rx_ready()) {
            v |= kRxReady;
        }
        if (tx_ready_d_) {
            v |= kTxReady;
        }
        return v;
    }

    unsigned rx_edges = 0;
    unsigned tx_edges = 0;

private:
    struct RxValue {
        std::uint32_t value;
        bool last;
    };
    enum class RxState { kStart, kMeta, kData };

    // The FIFO's fill as the state machine sees it: the read side's count,
    // registered on the Pi's RX clock, so it changes only on its edges.
    bool rx_ready() const { return rx_state_ == RxState::kStart && rx_seen_ > kRxStartAbove; }

    unsigned rx_nibble() const {
        switch (rx_state_) {
            case RxState::kStart:
                return rx_ready() ? 0u : rx_latch_;
            case RxState::kMeta:
                return rx_index_ & 0xfu;
            case RxState::kData:
                return (rx_current_.value >> (4 * (5 - rx_nibble_no_))) & 0xfu;
        }
        return 0;
    }

    void rx_edge() {
        ++rx_edges;
        rx_latch_ = rx_nibble();
        switch (rx_state_) {
            case RxState::kStart:
                if (rx_seen_ > kRxStartAbove) {
                    rx_current_ = rx_fifo_.front();
                    rx_fifo_.pop_front();
                    rx_state_ = RxState::kMeta;
                }
                break;
            case RxState::kMeta:
                rx_index_ = rx_current_.last ? 0 : rx_index_ + 1;
                rx_state_ = RxState::kData;
                rx_nibble_no_ = 0;
                break;
            case RxState::kData:
                if (++rx_nibble_no_ == 6) {
                    rx_state_ = RxState::kStart;
                }
                break;
        }
        rx_seen_ = rx_fifo_.size();
    }

    void tx_edge() {
        ++tx_edges;
        if (!tx_shifting_) {
            if (tx_ready_d_) {
                tx_shifting_ = true;
                tx_bits_ = 0;
                tx_word_ = 0;
            }
        } else {
            tx_word_ = (tx_word_ << 1) | ((out_ & kTxData) ? 1u : 0u);
            if (++tx_bits_ == 32) {
                tx_shifting_ = false;
                if ((tx_fifo_words_ + 1) * 4 <= kTxFifoBytes) {
                    ++tx_fifo_words_;
                    tx_received_.push_back(tx_word_);
                }
            }
        }
        tx_ready_d_ = (kTxFifoBytes - tx_fifo_words_ * 4) > kTxReadyMargin;
    }

    std::uint32_t out_ = 0;

    std::deque<RxValue> rx_fifo_;
    std::size_t rx_seen_ = 0;
    RxState rx_state_ = RxState::kStart;
    RxValue rx_current_{};
    unsigned rx_index_ = 0;
    unsigned rx_nibble_no_ = 0;
    unsigned rx_latch_ = 0;

    bool tx_ready_d_ = false;
    bool tx_shifting_ = false;
    unsigned tx_bits_ = 0;
    std::uint32_t tx_word_ = 0;
    std::size_t tx_fifo_words_ = 0;
    std::vector<std::uint32_t> tx_received_;
};

}  // namespace radioberry::test
