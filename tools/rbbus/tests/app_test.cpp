// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <vector>

#include "gateware_bus_sim.hpp"

using radioberry::Command;
using radioberry::test::GatewareBusSim;
using namespace radioberry::bus;

struct rb_bb_io {
    GatewareBusSim* sim;
};
static inline void rb_bb_set(struct rb_bb_io* io, uint32_t mask) { io->sim->set(mask); }
static inline void rb_bb_clr(struct rb_bb_io* io, uint32_t mask) { io->sim->clr(mask); }
static inline uint32_t rb_bb_lev(struct rb_bb_io* io) { return io->sim->lev(); }

#include "rb_bitbang.h"

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kGatewareRxFifo = 1012;  // values kept before it drops

// The simulated radio and time. Outlives the Bus and Control objects,
// which run_app() destroys before it returns.
struct Radio {
    GatewareBusSim sim;
    std::chrono::nanoseconds t{0};
    std::chrono::nanoseconds per_clock{100};
    unsigned receivers = 1;
    unsigned rate_hz = 0;  // 0 while run is clear
    std::uint64_t produced = 0;
    std::uint64_t rx_dropped = 0;
    std::size_t tx_drained = 0;
    std::vector<Command> sent;
    // CL025 gateware 75.3, one transmitter, six receivers.
    std::array<std::uint8_t, 6> reply{0,  0, 0, static_cast<std::uint8_t>(1 << 6 | 6 << 2 | 2),
                                      75, 3};
    BusCounters counters;

    // RX values at the commanded rate as time passes, dropped past the
    // gateware FIFO's 1012.
    void produce() {
        const auto due =
            static_cast<std::uint64_t>(std::chrono::duration<double>(t).count() * rate_hz);
        for (; rate_hz && produced < due; ++produced) {
            for (unsigned v = 0; v < 2 * receivers; ++v) {
                if (sim.rx_queued() >= kGatewareRxFifo) {
                    ++rx_dropped;
                    continue;
                }
                sim.push_value(static_cast<std::uint32_t>(produced * 16 + v),
                               v + 1 == 2 * receivers);
            }
        }
    }
    // The gateware reads its TX FIFO at 48 000 words a second.
    void drain_tx() {
        const auto due = static_cast<std::size_t>(std::chrono::duration<double>(t).count() * 48000);
        sim.tx_drain(due - tx_drained);
        tx_drained = due;
    }
};

class SimBus final : public Bus {
public:
    explicit SimBus(Radio& radio) : radio_(radio) { bus_.io = &io_; }
    ~SimBus() override { radio_.counters = counters(); }
    SimBus(const SimBus&) = delete;
    SimBus& operator=(const SimBus&) = delete;

    unsigned rx_run(std::uint32_t* out, unsigned max_words, unsigned max_idle) override {
        radio_.produce();
        const auto before = bus_.stats.rx_clocks;
        const unsigned n = rb_bb_rx_run(&bus_, out, max_words, max_idle);
        radio_.t += radio_.per_clock * static_cast<long>(bus_.stats.rx_clocks - before);
        return n;
    }
    unsigned tx_run(const std::uint32_t* words, unsigned count, unsigned max_polls) override {
        const auto before = bus_.stats.tx_clocks;
        const unsigned n = rb_bb_tx_run(&bus_, words, count, max_polls);
        radio_.t += radio_.per_clock * static_cast<long>(bus_.stats.tx_clocks - before);
        radio_.drain_tx();
        return n;
    }
    void set_settle_reads(unsigned reads) override { bus_.settle_reads = reads; }
    BusCounters counters() const override {
        const auto& s = bus_.stats;
        return {s.rx_clocks, s.rx_idle_clocks, s.rx_words,
                s.tx_clocks, s.tx_ready_waits, s.tx_words};
    }

private:
    Radio& radio_;
    rb_bb_io io_{&radio_.sim};
    rb_bb_bus bus_{};
};

// Records commands; a general command with run sets what the gateware
// produces, a command without run stops it.
class FakeControl final : public Control {
public:
    explicit FakeControl(Radio& radio) : radio_(radio) {}
    std::array<std::uint8_t, 6> transfer(const std::array<std::uint8_t, 6>& tx) override {
        const Command c = Command::decode(tx);
        radio_.sent.push_back(c);
        if (c.address == 0 && (c.flags & Command::kRun)) {
            radio_.receivers = ((c.data >> 3) & 0xf) + 1;
            radio_.rate_hz = 48000u << ((c.data >> 24) & 3);
        } else if (!(c.flags & Command::kRun)) {
            radio_.rate_hz = 0;
        }
        return radio_.reply;
    }

private:
    Radio& radio_;
};

struct Fixture {
    Radio radio;
    std::string compatible = std::string("raspberrypi,4-model-b\0brcm,bcm2711", 34) + '\0';
    bool driver_bound = false;
    bool bus_opened = false;
    std::ostringstream out, err;
    Environment env{
        .compatible = [this] { return compatible; },
        .driver_bound = [this] { return driver_bound; },
        .open_bus =
            [this] {
                bus_opened = true;
                return std::unique_ptr<Bus>(std::make_unique<SimBus>(radio));
            },
        .open_control =
            [this](const std::string&) {
                return std::unique_ptr<Control>(std::make_unique<FakeControl>(radio));
            },
        .now =
            [this] {
                return std::chrono::duration_cast<std::chrono::steady_clock::duration>(radio.t);
            },
        .sleep = [this](std::chrono::microseconds d) { radio.t += d; },
        .cpu_seconds = [this] { return std::chrono::duration<double>(radio.t).count() / 10; },
        .out = out,
        .err = err,
    };

    int run(std::vector<std::string> args) { return run_app(args, env); }
};

}  // namespace

TEST_CASE("rbbus options", "[rbbus]") {
    auto parse = [](std::vector<std::string> a) { return parse_options(a); };
    const auto d = std::get<Options>(parse({}));
    CHECK(d.seconds == 10);
    CHECK(d.receivers == 1);
    CHECK(d.rate_code == 0);
    CHECK(d.settle_reads == 0);
    CHECK(d.idle_sleep_us == 1000);
    CHECK_FALSE(d.tx);
    CHECK(d.spi == "/dev/spidev0.0");

    const auto o = std::get<Options>(
        parse({"--seconds", "2.5", "--receivers", "3", "--rate", "192", "--settle", "2",
               "--idle-sleep", "0", "--tx", "--spi", "/dev/spidev1.0", "--yaml"}));
    CHECK(o.seconds == 2.5);
    CHECK(o.receivers == 3);
    CHECK(o.rate_code == 2);
    CHECK(o.settle_reads == 2);
    CHECK(o.idle_sleep_us == 0);
    CHECK(o.tx);
    CHECK(o.spi == "/dev/spidev1.0");
    CHECK(o.format == ReportFormat::kYaml);
    CHECK(std::get<Options>(parse({"--json"})).format == ReportFormat::kJson);

    CHECK(std::holds_alternative<ShowHelp>(parse({"--help"})));
    CHECK(std::holds_alternative<ShowHelp>(parse({"-h"})));
    CHECK(std::holds_alternative<ShowVersion>(parse({"--version"})));
    for (const auto& bad : std::vector<std::vector<std::string>>{{"--seconds"},
                                                                 {"--seconds", "0"},
                                                                 {"--seconds", "x"},
                                                                 {"--seconds", "1x"},
                                                                 {"--receivers", "0"},
                                                                 {"--receivers", "7"},
                                                                 {"--receivers"},
                                                                 {"--rate", "50"},
                                                                 {"--rate"},
                                                                 {"--settle", "1001"},
                                                                 {"--settle", "-1"},
                                                                 {"--idle-sleep", "1000001"},
                                                                 {"--idle-sleep", "12345678901"},
                                                                 {"--spi"},
                                                                 {"--bogus"}}) {
        CHECK(std::holds_alternative<UsageError>(parse(bad)));
    }
}

TEST_CASE("rbbus help, version and usage errors", "[rbbus]") {
    Fixture f;
    CHECK(f.run({"--help"}) == kExitOk);
    CHECK(f.out.str().find("usage: rbbus") != std::string::npos);
    Fixture v;
    CHECK(v.run({"--version"}) == kExitOk);
    CHECK(v.out.str().starts_with("rbbus "));
    Fixture u;
    CHECK(u.run({"--rate", "7"}) == kExitUsage);
    CHECK(u.err.str().find("--rate") != std::string::npos);
}

TEST_CASE("rbbus runs only on a Pi 4 with the driver not bound", "[rbbus]") {
    Fixture pi5;
    pi5.compatible = std::string("raspberrypi,5-model-b\0brcm,bcm2712", 34) + '\0';
    CHECK(pi5.run({}) == kExitUsage);
    CHECK(pi5.err.str().find("not a Raspberry Pi 4") != std::string::npos);
    CHECK_FALSE(pi5.bus_opened);
    CHECK(pi5.radio.sent.empty());

    Fixture bound;
    bound.driver_bound = true;
    CHECK(bound.run({}) == kExitUsage);
    CHECK(bound.err.str().find("/dev/radioberry") != std::string::npos);
    CHECK_FALSE(bound.bus_opened);
}

TEST_CASE("rbbus at FT8's rate keeps up and never commands transmit", "[rbbus]") {
    Fixture f;
    REQUIRE(f.run({"--seconds", "2", "--json"}) == kExitOk);
    const std::string out = f.out.str();
    CHECK(out.find("\"pass\": true") != std::string::npos);
    CHECK(out.find("\"resyncs\": 0") != std::string::npos);
    CHECK(out.find("\"rate_ksps\": 48") != std::string::npos);
    CHECK(out.find("\"version\": \"75.3\"") != std::string::npos);
    CHECK(f.radio.rx_dropped == 0);
    REQUIRE(f.radio.sent.size() == 3);
    CHECK(f.radio.sent.front() == safe_command());
    CHECK(f.radio.sent[1] == radioberry::hl2::general(0, 1));
    CHECK(f.radio.sent.back() == safe_command());
    for (const Command& c : f.radio.sent) {
        CHECK_FALSE(c.can_transmit());
        CHECK(radioberry::transmit_hazards(c).empty());
    }
    // The gateware's bursts leave idle time, slept rather than spun.
    CHECK(f.radio.counters.rx_idle_clocks > 0);
    CHECK(f.radio.counters.rx_words >= 2 * 48000 * 2 * 98 / 100);
}

TEST_CASE("rbbus reports lost frames when the bus is too slow", "[rbbus]") {
    Fixture f;
    f.radio.per_clock = 2000ns;  // 0.5 MHz: 62 500 values a second at best
    CHECK(f.run({"--seconds", "1", "--receivers", "2", "--rate", "96", "--yaml"}) == kExitFailed);
    CHECK(f.radio.rx_dropped > 0);
    CHECK(f.out.str().find("pass: false") != std::string::npos);
}

TEST_CASE("rbbus clocks TX words with PTT clear", "[rbbus]") {
    Fixture f;
    REQUIRE(f.run({"--seconds", "1", "--tx", "--yaml"}) == kExitOk);
    CHECK(f.out.str().find("tx:") != std::string::npos);
    const auto& got = f.radio.sim.tx_received();
    CHECK(got.size() > 47000);
    CHECK(std::all_of(got.begin(), got.end(), [](std::uint32_t w) { return w == 0; }));
    for (const Command& c : f.radio.sent) {
        CHECK_FALSE(c.can_transmit());
    }
}

TEST_CASE("rbbus stops when no gateware answers or it has too few receivers", "[rbbus]") {
    Fixture none;
    none.radio.reply = {};
    CHECK(none.run({"--seconds", "1"}) == kExitFailed);
    CHECK(none.err.str().find("rbload") != std::string::npos);
    // It polled for about 10 s, sending only the safe command.
    CHECK(none.radio.sent.size() > 90);
    CHECK(none.radio.sent.size() < 110);
    for (const Command& c : none.radio.sent) {
        CHECK(c == safe_command());
    }

    Fixture six;
    CHECK(six.run({"--seconds", "1", "--receivers", "6"}) == kExitOk);

    Fixture cl016;
    cl016.radio.reply[3] = static_cast<std::uint8_t>(1 << 6 | 4 << 2 | 1);  // four receivers
    CHECK(cl016.run({"--receivers", "5"}) == kExitFailed);
    CHECK(cl016.err.str().find("4 receivers") != std::string::npos);
}

TEST_CASE("rbbus waits for a freshly loaded gateware to answer", "[rbbus]") {
    Fixture f;
    const auto loaded = f.radio.reply;
    f.radio.reply = {};
    // The gateware answers from 3 s on.
    f.env.sleep = [&f, loaded](std::chrono::microseconds d) {
        f.radio.t += d;
        if (f.radio.t >= std::chrono::seconds(3)) {
            f.radio.reply = loaded;
        }
    };
    CHECK(f.run({"--seconds", "1", "--yaml"}) == kExitOk);
    CHECK(f.out.str().find("75.3") != std::string::npos);
}

TEST_CASE("rbbus reports a device that cannot be opened", "[rbbus]") {
    Fixture f;
    f.env.open_control = [](const std::string& d) -> std::unique_ptr<Control> {
        throw std::runtime_error("cannot open " + d);
    };
    CHECK(f.run({"--spi", "/dev/spidev9.9"}) == kExitFailed);
    CHECK(f.err.str().find("cannot open /dev/spidev9.9") != std::string::npos);
}

TEST_CASE("rbbus sends the safe command even when the run is interrupted", "[rbbus]") {
    Fixture f;
    f.env.sleep = [](std::chrono::microseconds d) {
        if (d >= std::chrono::microseconds(1000)) {
            throw std::runtime_error("interrupted");
        }
    };
    f.radio.reply[3] = static_cast<std::uint8_t>(1 << 6 | 6 << 2 | 2);
    CHECK(f.run({"--seconds", "1"}) == kExitFailed);
    REQUIRE_FALSE(f.radio.sent.empty());
    CHECK(f.radio.sent.back() == safe_command());
    CHECK(f.err.str().find("interrupted") != std::string::npos);
}
