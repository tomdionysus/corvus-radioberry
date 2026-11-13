// SPDX-License-Identifier: GPL-2.0-or-later
//
// rbsmi and driver/kernel/rb_smi.h against the gateware bus simulator.
// SMI latches the pins at the end of each read strobe, before the strobe's
// rising edge advances the gateware, so the simulated reader samples and
// then clocks: the opposite order to the CPU transport, which clocks and
// then samples.
#include "app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <vector>

#include "gateware_bus_sim.hpp"
#include "rb_smi.h"

using radioberry::Command;
using radioberry::test::GatewareBusSim;
using namespace radioberry::smi;

namespace {

using namespace std::chrono_literals;

// The bus pins as GPIO numbers, as the simulator takes them.
constexpr std::uint32_t kRxClk = 1u << 6;
constexpr std::uint32_t kRxReady = 1u << 25;
constexpr unsigned kRxD0 = 18;
constexpr std::size_t kGatewareRxFifo = 1012;  // values kept before it drops

// The pins as SMI stores them in an 18-bit read: SD n is GPIO n + 8.
std::uint32_t smi_word(std::uint32_t lev) {
    return ((lev >> kRxD0) & 0xfu) << RB_SMI_D0_SHIFT | ((lev & kRxReady) ? RB_SMI_READY : 0);
}

struct Radio {
    GatewareBusSim sim;
    std::chrono::nanoseconds t{0};
    unsigned receivers = 1;
    unsigned rate_hz = 0;  // 0 while run is clear
    std::uint64_t produced = 0;
    std::uint64_t rx_dropped = 0;
    std::vector<Command> sent;
    std::array<std::uint8_t, 6> reply{0,  0, 0, static_cast<std::uint8_t>(1 << 6 | 6 << 2 | 2),
                                      75, 3};
    std::optional<Timing> configured;
    // Strobes lost between reads: the clock stops while no read is active.
    std::chrono::nanoseconds gap_per_read{0};
    // From this strobe on, the reader misses the next sample-ready word,
    // which loses that value; 0 for none.
    std::uint64_t skip_after = 0;
    bool skipped = false;
    std::uint64_t strobes = 0;

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
};

class SimReader final : public Reader {
public:
    explicit SimReader(Radio& radio) : radio_(radio) {}
    void configure(const Timing& timing) override {
        radio_.configured = timing;
        per_strobe_ = std::chrono::nanoseconds(8 * timing.cycles());
    }
    std::size_t read(std::span<std::uint32_t> words) override {
        radio_.t += radio_.gap_per_read;
        std::size_t n = 0;
        while (n < words.size()) {
            if (radio_.strobes % 64 == 0) {
                radio_.produce();
            }
            const std::uint32_t w = smi_word(radio_.sim.lev());
            radio_.sim.set(kRxClk);
            radio_.sim.clr(kRxClk);
            radio_.t += per_strobe_;
            if (radio_.skip_after && ++radio_.strobes >= radio_.skip_after && !radio_.skipped &&
                (w & RB_SMI_READY)) {
                radio_.skipped = true;
                continue;
            }
            if (!radio_.skip_after) {
                ++radio_.strobes;
            }
            words[n++] = w;
        }
        return n;
    }

private:
    Radio& radio_;
    std::chrono::nanoseconds per_strobe_{240};
};

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
    bool reader_opened = false;
    std::ostringstream out, err;
    Environment env{
        .compatible = [this] { return compatible; },
        .driver_bound = [this] { return driver_bound; },
        .open_reader =
            [this] {
                reader_opened = true;
                return std::unique_ptr<Reader>(std::make_unique<SimReader>(radio));
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
        .cpu_seconds = [this] { return std::chrono::duration<double>(radio.t).count() / 20; },
        .out = out,
        .err = err,
    };

    int run(std::vector<std::string> args) { return run_app(args, env); }
};

}  // namespace

TEST_CASE("the SMI parser rebuilds values across buffers and counts idle strobes", "[rbsmi]") {
    // Two values, 0x1abcdef (index 1) and 0x0123456 (index 0), with idle
    // strobes around them, split at every possible point.
    std::vector<std::uint32_t> words;
    const auto idle = [&](int n) {
        for (int i = 0; i < n; ++i) {
            words.push_back(0x1c3ffu);  // SD0..SD9 and SD14..SD16 set, ours low
        }
    };
    const auto value = [&](std::uint32_t v) {
        words.push_back(RB_SMI_READY | 0xfu << RB_SMI_D0_SHIFT);
        for (int i = 6; i >= 0; --i) {
            words.push_back(((v >> (4 * i)) & 0xfu) << RB_SMI_D0_SHIFT | 0x3u);
        }
    };
    idle(3);
    value(0x1abcdef);
    idle(5);
    value(0x0123456);
    idle(2);
    for (std::size_t split = 0; split <= words.size(); ++split) {
        rb_smi_parser p;
        rb_smi_init(&p);
        std::vector<std::uint32_t> out(rb_smi_max_values(static_cast<unsigned>(words.size())));
        unsigned got = rb_smi_parse(&p, words.data(), static_cast<unsigned>(split), out.data());
        got += rb_smi_parse(&p, words.data() + split, static_cast<unsigned>(words.size() - split),
                            out.data() + got);
        REQUIRE(got == 2);
        CHECK(out[0] == 0x1abcdefu);
        CHECK(out[1] == 0x0123456u);
        CHECK(p.idle == 10);
        CHECK(p.strobes == words.size());
        CHECK(p.values == 2);
    }
    CHECK(rb_smi_max_values(0) == 1);
    CHECK(rb_smi_max_values(16) == 3);
}

TEST_CASE("rbsmi options", "[rbsmi]") {
    auto parse = [](std::vector<std::string> a) { return parse_options(a); };
    const auto d = std::get<Options>(parse({}));
    CHECK(d.seconds == 10);
    CHECK(d.receivers == 1);
    CHECK(d.rate_code == 0);
    CHECK(d.timing.cycles() == 30);
    CHECK(d.read_words == 16384);
    CHECK(d.spi == "/dev/spidev0.0");

    const auto o = std::get<Options>(
        parse({"--seconds", "2.5", "--receivers", "3", "--rate", "192", "--timing", "1,5,0,4",
               "--read-words", "4096", "--spi", "/dev/spidev1.0", "--yaml"}));
    CHECK(o.seconds == 2.5);
    CHECK(o.receivers == 3);
    CHECK(o.rate_code == 2);
    CHECK(o.timing.setup == 1);
    CHECK(o.timing.strobe == 5);
    CHECK(o.timing.hold == 0);
    CHECK(o.timing.pace == 4);
    CHECK(o.read_words == 4096);
    CHECK(o.spi == "/dev/spidev1.0");
    CHECK(o.format == ReportFormat::kYaml);
    CHECK(std::get<Options>(parse({"--json"})).format == ReportFormat::kJson);

    CHECK(std::holds_alternative<ShowHelp>(parse({"--help"})));
    CHECK(std::holds_alternative<ShowVersion>(parse({"--version"})));
    for (const auto& bad : std::vector<std::vector<std::string>>{{"--seconds", "0"},
                                                                 {"--receivers", "7"},
                                                                 {"--rate", "50"},
                                                                 {"--timing", "0,5,0,4"},
                                                                 {"--timing", "1,128,0,4"},
                                                                 {"--timing", "1,5,64,4"},
                                                                 {"--timing", "1,5,0,128"},
                                                                 {"--timing", "1,5,0"},
                                                                 {"--timing", "1,5,0,4,1"},
                                                                 {"--timing", "a,b,c,d"},
                                                                 {"--timing"},
                                                                 {"--read-words", "63"},
                                                                 {"--read-words", "1048577"},
                                                                 {"--spi"},
                                                                 {"--bogus"}}) {
        CHECK(std::holds_alternative<UsageError>(parse(bad)));
    }
}

TEST_CASE("rbsmi runs only on a Pi 4 with the driver not bound", "[rbsmi]") {
    Fixture pi5;
    pi5.compatible = std::string("raspberrypi,5-model-b\0brcm,bcm2712", 34) + '\0';
    CHECK(pi5.run({}) == kExitUsage);
    CHECK(pi5.err.str().find("not a Raspberry Pi 4") != std::string::npos);
    CHECK_FALSE(pi5.reader_opened);
    CHECK(pi5.radio.sent.empty());

    Fixture bound;
    bound.driver_bound = true;
    CHECK(bound.run({}) == kExitUsage);
    CHECK(bound.err.str().find("/dev/radioberry") != std::string::npos);
    CHECK_FALSE(bound.reader_opened);

    Fixture help;
    CHECK(help.run({"--help"}) == kExitOk);
    CHECK(help.out.str().find("usage: rbsmi") != std::string::npos);
}

TEST_CASE("rbsmi reads one receiver at 192 ksps whole and never commands transmit", "[rbsmi]") {
    Fixture f;
    REQUIRE(f.run({"--seconds", "1", "--rate", "192", "--json"}) == kExitOk);
    const std::string out = f.out.str();
    CHECK(out.find("\"pass\": true") != std::string::npos);
    CHECK(out.find("\"resyncs\": 0") != std::string::npos);
    CHECK(out.find("\"bits_seen\": \"0x23c00\"") != std::string::npos);
    CHECK(f.radio.rx_dropped == 0);
    REQUIRE(f.radio.configured);
    CHECK(f.radio.configured->cycles() == 30);
    REQUIRE(f.radio.sent.size() == 3);
    CHECK(f.radio.sent[1] == radioberry::hl2::general(2, 1));
    for (const Command& c : f.radio.sent) {
        CHECK_FALSE(c.can_transmit());
        CHECK(radioberry::transmit_hazards(c).empty());
    }
}

TEST_CASE("rbsmi fails when the strobes are too slow or a strobe is lost", "[rbsmi]") {
    // 384 ksps for one receiver needs 6.1 million strobes a second; 127+127
    // cycles give under half a million, so the gateware's FIFO overflows.
    Fixture slow;
    CHECK(slow.run({"--seconds", "0.2", "--rate", "384", "--timing", "1,127,0,127", "--json"}) ==
          kExitFailed);
    CHECK(slow.radio.rx_dropped > 0);
    CHECK(slow.out.str().find("\"pass\": false") != std::string::npos);

    // A value lost in the middle of the stream breaks the frame sequence.
    Fixture lost;
    lost.radio.skip_after = 300000;
    CHECK(lost.run({"--seconds", "0.5", "--json"}) == kExitFailed);
    CHECK(lost.radio.skipped);
    CHECK(lost.out.str().find("\"resyncs\": 1") != std::string::npos);

    // A pause between reads shorter than the gateware's FIFO lasts is
    // absorbed.
    Fixture gaps;
    gaps.radio.gap_per_read = 1ms;
    CHECK(gaps.run({"--seconds", "1", "--rate", "192"}) == kExitOk);
    CHECK(gaps.radio.rx_dropped == 0);
}
