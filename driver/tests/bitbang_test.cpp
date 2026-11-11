// SPDX-License-Identifier: GPL-2.0-or-later
//
// The CPU-clocked bus engine (driver/kernel/rb_bitbang.h) against a
// simulation of the gateware's bus logic.
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>
#include <vector>

#include "gateware_bus_sim.hpp"

using radioberry::test::GatewareBusSim;

// Register access for rb_bitbang.h: the simulator stands in for GPSET0,
// GPCLR0 and GPLEV0.
struct rb_bb_io {
    GatewareBusSim* sim;
};
static inline void rb_bb_set(struct rb_bb_io* io, uint32_t mask) { io->sim->set(mask); }
static inline void rb_bb_clr(struct rb_bb_io* io, uint32_t mask) { io->sim->clr(mask); }
static inline uint32_t rb_bb_lev(struct rb_bb_io* io) { return io->sim->lev(); }

#include "rb_bitbang.h"

namespace {

struct Harness {
    GatewareBusSim sim;
    rb_bb_io io{&sim};
    rb_bb_bus bus{&io, 0, {}};
};

// A frame of 2 x receivers values, distinct per frame and value, some with
// the sign bit set.
std::vector<std::uint32_t> frame(unsigned receivers, unsigned n) {
    std::vector<std::uint32_t> v;
    for (unsigned i = 0; i < 2 * receivers; ++i) {
        v.push_back(((n * 16 + i) * 0x010203u + (i % 2 ? 0x800000u : 0)) & 0xffffff);
    }
    return v;
}

}  // namespace

TEST_CASE("RX: words are the PIO program's index << 24 | value", "[driver][bitbang]") {
    Harness h;
    for (unsigned n = 0; n < 10; ++n) {
        h.sim.push_frame(frame(2, n));
    }
    std::vector<u32> words(64);
    const unsigned got = rb_bb_rx_run(&h.bus, words.data(), 64, 16);
    // The gateware stops presenting values at 12 left in its FIFO.
    REQUIRE(got == 40 - 12);
    for (unsigned w = 0; w < got; ++w) {
        const auto f = frame(2, w / 4);
        CHECK(words[w] == (((w % 4) << 24) | f[w % 4]));
    }
    CHECK(h.sim.rx_queued() == 12);
    CHECK(h.bus.stats.rx_words == got);
    CHECK(h.bus.stats.rx_idle_clocks == 16);
    CHECK(h.bus.stats.rx_clocks == got * 8 + 16);
    CHECK(h.sim.rx_edges == h.bus.stats.rx_clocks);
    CHECK((h.sim.lev() & GatewareBusSim::kRxClk) == 0);  // clock low on return
}

TEST_CASE("RX: frames reassemble through the driver's parser", "[driver][bitbang]") {
    Harness h;
    rb_rx_parser p;
    rb_rx_init(&p, 3);
    std::vector<u32> words(50);
    unsigned frames = 0;
    for (unsigned n = 0;; ++n) {
        if (n < 200) {
            h.sim.push_frame(frame(3, n));
        }
        // Uneven reads: the bus stops and starts between frames and values.
        const unsigned got = rb_bb_rx_run(&h.bus, words.data(), 1 + n % 7, 4);
        if (n >= 200 && got == 0) {
            break;
        }
        for (unsigned i = 0; i < got; ++i) {
            if (rb_rx_feed(&p, words[i])) {
                const auto f = frame(3, frames);
                for (unsigned v = 0; v < 6; ++v) {
                    const u32 value = (u32(p.frame[3 * v]) << 16) | (u32(p.frame[3 * v + 1]) << 8) |
                                      p.frame[3 * v + 2];
                    CHECK(value == f[v]);
                }
                ++frames;
            }
        }
    }
    // All but the 12 values (two frames) the gateware keeps back.
    CHECK(frames == 198);
    CHECK(h.sim.rx_queued() == 12);
    CHECK(p.resyncs == 0);
    CHECK(p.dropped == 0);
}

TEST_CASE("RX: a nearly empty gateware FIFO costs only idle clocks", "[driver][bitbang]") {
    Harness h;
    h.sim.push_frame(frame(6, 0));  // 12 values: not enough to start
    std::vector<u32> words(8);
    CHECK(rb_bb_rx_run(&h.bus, words.data(), 8, 100) == 0);
    CHECK(h.bus.stats.rx_idle_clocks == 100);
    CHECK(h.sim.rx_queued() == 12);
    CHECK(rb_bb_rx_run(&h.bus, words.data(), 0, 100) == 0);  // nothing asked for
}

TEST_CASE("RX: settling reads change nothing but time", "[driver][bitbang]") {
    Harness h;
    h.bus.settle_reads = 3;
    for (unsigned n = 0; n < 8; ++n) {
        h.sim.push_frame(frame(1, n));
    }
    std::vector<u32> words(16);
    const unsigned got = rb_bb_rx_run(&h.bus, words.data(), 16, 8);
    REQUIRE(got == 4);
    for (unsigned w = 0; w < got; ++w) {
        CHECK(words[w] == (((w % 2) << 24) | frame(1, w / 2)[w % 2]));
    }
}

TEST_CASE("TX: words arrive whole and in order", "[driver][bitbang]") {
    Harness h;
    std::vector<u32> words;
    for (u32 i = 0; i < 1000; ++i) {
        words.push_back(i * 0x9e3779b9u);
    }
    // The registered ready is low until the first edge.
    CHECK(rb_bb_tx_run(&h.bus, words.data(), static_cast<unsigned>(words.size()), 4) == 1000);
    CHECK(h.sim.tx_received() == words);
    CHECK(h.bus.stats.tx_words == 1000);
    CHECK(h.bus.stats.tx_ready_waits == 1);
    CHECK(h.bus.stats.tx_clocks == 1000 * 33 + 1);
    CHECK(h.sim.tx_edges == h.bus.stats.tx_clocks);
    CHECK((h.sim.lev() & GatewareBusSim::kTxClk) == 0);
}

TEST_CASE("TX: a full gateware FIFO stops the run without losing words", "[driver][bitbang]") {
    Harness h;
    const unsigned capacity = GatewareBusSim::kTxFifoBytes / 4;
    std::vector<u32> words(capacity + 100);
    for (u32 i = 0; i < words.size(); ++i) {
        words[i] = i;
    }
    const unsigned first =
        rb_bb_tx_run(&h.bus, words.data(), static_cast<unsigned>(words.size()), 8);
    // Ready falls 8 bytes before full.
    CHECK(first == capacity - 2);
    CHECK(h.sim.tx_fifo_words() == capacity - 2);
    h.sim.tx_drain(500);
    const unsigned rest =
        rb_bb_tx_run(&h.bus, words.data() + first, static_cast<unsigned>(words.size()) - first, 8);
    CHECK(rest == words.size() - first);
    CHECK(h.sim.tx_received() == words);
}

TEST_CASE("RX and TX interleaved at random", "[driver][bitbang]") {
    Harness h;
    std::mt19937 engine(7);
    auto rng = [&] { return static_cast<unsigned>(engine()); };
    rb_rx_parser p;
    rb_rx_init(&p, 2);
    std::vector<u32> rx(32), sent;
    unsigned pushed = 0, frames = 0;
    u32 next_tx = 1;
    for (int round = 0; round < 2000; ++round) {
        switch (rng() % 3) {
            case 0:
                h.sim.push_frame(frame(2, pushed++));
                break;
            case 1: {
                const unsigned got = rb_bb_rx_run(&h.bus, rx.data(), 1 + rng() % 32, 1 + rng() % 9);
                for (unsigned i = 0; i < got; ++i) {
                    if (rb_rx_feed(&p, rx[i])) {
                        CHECK(p.frame[2] == (frame(2, frames)[0] & 0xff));
                        ++frames;
                    }
                }
                break;
            }
            default: {
                std::vector<u32> batch(1 + rng() % 20);
                for (auto& w : batch) {
                    w = next_tx++;
                }
                const unsigned n =
                    rb_bb_tx_run(&h.bus, batch.data(), static_cast<unsigned>(batch.size()), 4);
                sent.insert(sent.end(), batch.begin(), batch.begin() + n);
                next_tx -= static_cast<u32>(batch.size() - n);
                h.sim.tx_drain(rng() % 30);
            }
        }
    }
    CHECK(frames > 100);
    CHECK(p.resyncs == 0);
    CHECK(h.sim.tx_received() == sent);
}
