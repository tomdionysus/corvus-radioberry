// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <complex>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "fake_device.hpp"
#include "log_capture.hpp"

using namespace radioberry;
using namespace radioberry::soapy;
using radioberry::daemon::test::FakeDevice;
using namespace std::chrono_literals;

namespace {

constexpr int RX = SOAPY_SDR_RX;
constexpr int TX = SOAPY_SDR_TX;

bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds limit = 2000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

Timing fast() {
    Timing t;
    t.tx_tail = 10ms;
    t.tx_tail_max = 40ms;
    t.tx_tick = 2ms;
    t.tx_drain = 150ms;
    return t;
}

Settings transmit_settings() {
    Settings s;
    s.tx.allow_tx = true;
    s.tx.allow_pa = true;
    s.tx.drive_ceiling = 150;
    return s;
}

// The gateware's sample counter the radio sees.
enum class Counter {
    Drained,    // the samples drained from the fake's TX ring: taken at once
    Absent,     // no counter
    Stuck,      // a counter that never moves
    Vanishing,  // read once, at the burst's start, then unreadable
    Sysfs,      // the module's own reader, with no driver attribute to read
};

// A radio over a fake device the test keeps a handle on.
struct Rig {
    FakeDevice* fake;
    std::unique_ptr<Radio> radio;

    explicit Rig(const Settings& settings = {}, const Timing& timing = fast(),
                 const std::function<void(FakeDevice&)>& setup = {},
                 Counter counter = Counter::Drained) {
        auto device = std::make_unique<FakeDevice>();
        device->idle_wait_ms = 1;
        if (setup) {
            setup(*device);
        }
        fake = device.get();
        Radio::TxCounter count;
        switch (counter) {
            case Counter::Drained:
                count = [f = fake]() -> std::optional<std::uint32_t> {
                    const std::lock_guard lock(f->mutex);
                    return static_cast<std::uint32_t>(f->stats_.tx_words);
                };
                break;
            case Counter::Absent:
                count = [] { return std::optional<std::uint32_t>(); };
                break;
            case Counter::Stuck:
                count = [] { return std::optional<std::uint32_t>(7); };
                break;
            case Counter::Vanishing:
                count = [n = 0]() mutable {
                    return n++ == 0 ? std::optional<std::uint32_t>(7) : std::nullopt;
                };
                break;
            case Counter::Sysfs:
                break;
        }
        // The module's own reader, pointed at a directory with no driver
        // attribute, whatever the machine running the test has loaded.
        Settings s = settings;
        if (counter == Counter::Sysfs) {
            s.sysfs_dir = "/nonexistent/radioberry-test";
        }
        radio = std::make_unique<Radio>(std::move(device), s, timing, count);
    }

    std::vector<Command> commands() const {
        const std::lock_guard lock(fake->mutex);
        return fake->commands;
    }
    // The last command sent to `address`.
    std::optional<Command> last(std::uint8_t address) const {
        const auto all = commands();
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->address == address) {
                return *it;
            }
        }
        return std::nullopt;
    }
    bool sent(const std::function<bool(const Command&)>& match) const {
        return wait_until([&] {
            const auto all = commands();
            return std::any_of(all.begin(), all.end(), match);
        });
    }
    // The command most recently sent carries these transmit bits.
    bool keyed_now() const {
        const auto all = commands();
        return !all.empty() && all.back().ptt;
    }
    void frames(std::size_t n) {
        const std::lock_guard lock(fake->mutex);
        fake->frames_ready += n;
    }
    // Every frame given to the fake has been read.
    bool frames_read() const {
        const std::lock_guard lock(fake->mutex);
        return fake->frames_ready == 0;
    }
    void drain() {
        const std::lock_guard lock(fake->mutex);
        fake->drain_tx(fake->stats_.tx_ring_words);
    }
    std::size_t written_samples() const {
        const std::lock_guard lock(fake->mutex);
        return fake->written.size() / kTxSampleBytes;
    }
};

std::uint32_t corrected(double hz, double ppm) {
    return static_cast<std::uint32_t>(std::lround(hz / (1 + ppm * 1e-6)));
}

}  // namespace

TEST_CASE("a new radio is set up and idle") {
    Settings settings;
    settings.frequency_ppm = 10;
    settings.lna_gain_db = 20;
    const Rig rig(settings);
    REQUIRE(rig.sent([](const Command& c) { return c.address == 0x0a; }));
    CHECK(rig.last(0x00)->data == hl2::general(0, 1).data);
    CHECK(rig.last(0x02)->data == corrected(7.074e6, 10));
    CHECK(rig.last(0x07)->data == corrected(7.074e6, 10));  // the sixth receiver
    CHECK(rig.last(0x01)->data == corrected(7.074e6, 10));
    CHECK(rig.last(0x0a)->data == hl2::lna_gain(20).data);
    for (const Command& c : rig.commands()) {
        CHECK_FALSE(c.can_transmit());
        CHECK((c.flags & Command::kRun) == 0);
    }
    CHECK_FALSE(rig.radio->transmitting());
}

TEST_CASE("identification and channels") {
    const Rig rig({}, fast(), [](FakeDevice& d) { d.info_.transport = RADIOBERRY_TRANSPORT_PIO; });
    Radio& r = *rig.radio;
    CHECK(r.getDriverKey() == "radioberry");
    CHECK(r.getHardwareKey() == "RadioBerry 2.x");
    const auto info = r.getHardwareInfo();
    CHECK(info.at("gateware_version") == "75.3");
    CHECK(info.at("fpga") == "10CL025");
    CHECK(info.at("receivers") == "6");
    CHECK(info.at("transmitters") == "1");
    CHECK(info.at("transport") == "rp1-pio");
    CHECK(info.at("transmit") == "not allowed");
    CHECK(info.at("config") == "none (receive only)");
    CHECK(r.getNumChannels(RX) == 6);
    CHECK(r.getNumChannels(TX) == 1);
    CHECK(r.getFullDuplex(RX, 5));
    CHECK_THROWS_AS(r.getFullDuplex(RX, 6), std::invalid_argument);
    CHECK_THROWS_AS(r.getNumChannels(7), std::invalid_argument);
}

TEST_CASE("the transport is named") {
    for (const auto& [code, name] :
         std::vector<std::pair<unsigned, std::string>>{{RADIOBERRY_TRANSPORT_I2S, "i2s"},
                                                       {RADIOBERRY_TRANSPORT_CPU, "cpu"},
                                                       {9, "unknown"}}) {
        Settings settings = transmit_settings();
        settings.from_file = true;
        settings.path = "/etc/test.yaml";
        const Rig rig(settings, fast(),
                      [&](FakeDevice& d) { d.info_.transport = static_cast<std::uint8_t>(code); });
        const auto info = rig.radio->getHardwareInfo();
        CHECK(info.at("transport") == name);
        CHECK(info.at("transmit") == "allowed");
        CHECK(info.at("config") == "/etc/test.yaml");
    }
}

TEST_CASE("a receive-only image") {
    const Rig rig({}, fast(), [](FakeDevice& d) { d.info_.transmitters = 0; });
    Radio& r = *rig.radio;
    CHECK(r.getNumChannels(TX) == 0);
    CHECK_THROWS_AS(r.setupStream(TX, "CF32", {}, {}), std::invalid_argument);
    CHECK_THROWS_AS(r.setGain(TX, 0, 1.0), std::invalid_argument);
    r.setFrequencyCorrection(RX, 0, 1.0);
}

TEST_CASE("a device that fails at start") {
    CHECK_THROWS_AS(Rig({}, fast(), [](FakeDevice& d) { d.fail_on = "stats"; }),
                    std::runtime_error);
}

TEST_CASE("stream formats") {
    const Rig rig;
    Radio& r = *rig.radio;
    CHECK(r.getStreamFormats(RX, 0) == std::vector<std::string>{"CF32", "CS16", "CS32"});
    CHECK(r.getStreamFormats(TX, 0) == std::vector<std::string>{"CF32", "CS16"});
    double full_scale = 0;
    CHECK(r.getNativeStreamFormat(RX, 0, full_scale) == "CS32");
    CHECK(full_scale == kRxFullScale);
    CHECK(r.getNativeStreamFormat(TX, 0, full_scale) == "CS16");
    CHECK(full_scale == kTxFullScale);
}

TEST_CASE("antennas") {
    const Rig rig;
    Radio& r = *rig.radio;
    CHECK(r.listAntennas(RX, 0) == std::vector<std::string>{"RX"});
    CHECK(r.getAntenna(TX, 0) == "TX");
    r.setAntenna(RX, 1, "RX");
    r.setAntenna(RX, 1, "");
    CHECK_THROWS_AS(r.setAntenna(RX, 1, "TX"), std::invalid_argument);
}

TEST_CASE("frequencies and their correction") {
    const Rig rig;
    Radio& r = *rig.radio;
    CHECK(r.listFrequencies(RX, 0) == std::vector<std::string>{"RF"});
    const auto range = r.getFrequencyRange(TX, 0);
    REQUIRE(range.size() == 1);
    CHECK(range[0].maximum() == 38.4e6);
    CHECK_THROWS_AS(r.getFrequencyRange(RX, 0, "IF"), std::invalid_argument);

    r.setFrequency(RX, 2, 14.074e6, {});
    r.setFrequency(TX, 0, "RF", 14.076e6, {});
    CHECK(r.getFrequency(RX, 2) == 14.074e6);
    CHECK(r.getFrequency(TX, 0, "RF") == 14.076e6);
    CHECK(rig.sent([](const Command& c) { return c.address == 0x04 && c.data == 14074000; }));
    CHECK(rig.sent([](const Command& c) { return c.address == 0x01 && c.data == 14076000; }));
    // Receive: clamped into the range. Transmit: refused.
    r.setFrequency(RX, 0, 100e6, {});
    CHECK(r.getFrequency(RX, 0) == 38.4e6);
    r.setFrequency(RX, 0, -1, {});
    CHECK(r.getFrequency(RX, 0) == 0);
    CHECK_THROWS_AS(r.setFrequency(TX, 0, 40e6, {}), std::invalid_argument);
    CHECK_THROWS_AS(r.setFrequency(TX, 0, -1, {}), std::invalid_argument);
    CHECK_THROWS_AS(r.setFrequency(RX, 0, std::nan(""), {}), std::invalid_argument);
    CHECK(r.getFrequency(TX, 0) == 14.076e6);
    CHECK_THROWS_AS(r.setFrequency(RX, 0, "IF", 1e6, {}), std::invalid_argument);
    CHECK_THROWS_AS(r.getFrequency(RX, 0, "IF"), std::invalid_argument);
    CHECK_THROWS_AS(r.setFrequency(RX, 6, 1e6, {}), std::invalid_argument);

    CHECK(r.hasFrequencyCorrection(RX, 0));
    CHECK(r.getFrequencyCorrection(TX, 0) == 0);
    r.setFrequencyCorrection(RX, 0, 22.9);
    CHECK(r.getFrequencyCorrection(RX, 3) == 22.9);
    const auto want = corrected(14.074e6, 22.9);
    CHECK(rig.sent([&](const Command& c) { return c.address == 0x04 && c.data == want; }));
    CHECK_THROWS_AS(r.setFrequencyCorrection(RX, 0, 300), std::invalid_argument);
}

TEST_CASE("receive gain") {
    const Rig rig;
    Radio& r = *rig.radio;
    CHECK(r.listGains(RX, 0) == std::vector<std::string>{"LNA"});
    CHECK(r.getGain(RX, 0) == 14);
    CHECK(r.getGainRange(RX, 0).minimum() == -12);
    CHECK(r.getGainRange(RX, 0).maximum() == 48);
    r.setGain(RX, 1, 30.4);
    CHECK(r.getGain(RX, 0, "LNA") == 30);
    CHECK(rig.sent(
        [](const Command& c) { return c.address == 0x0a && c.data == hl2::lna_gain(30).data; }));
    r.setGain(RX, 0, "LNA", 99);
    CHECK(r.getGain(RX, 0) == 48);
    CHECK_THROWS_AS(r.setGain(RX, 0, "DRIVE", 1), std::invalid_argument);
    CHECK_THROWS_AS(r.getGain(RX, 0, "DRIVE"), std::invalid_argument);
    CHECK_THROWS_AS(r.getGainRange(RX, 0, "DRIVE"), std::invalid_argument);
}

TEST_CASE("transmit drive in dB at the AD9866, within the ceiling") {
    const Rig rig(transmit_settings());
    Radio& r = *rig.radio;
    CHECK(r.listGains(TX, 0) == std::vector<std::string>{"DRIVE"});
    CHECK(r.getGain(TX, 0) == 0);
    CHECK(r.getGainRange(TX, 0).maximum() == 11.5);  // ceiling 150 is code 23
    CHECK(r.getGainRange(TX, 0).step() == 0.5);
    r.setGain(TX, 0, 10.0);
    CHECK(r.getGain(TX, 0) == 10.0);
    r.setGain(TX, 0, "DRIVE", 30.0);
    CHECK(r.getGain(TX, 0) == 11.5);
    // Nothing is keyed by a gain change outside a burst.
    for (const Command& c : rig.commands()) {
        CHECK_FALSE(c.can_transmit());
    }
}

TEST_CASE("sample rates") {
    const Rig rig;
    Radio& r = *rig.radio;
    CHECK(r.listSampleRates(RX, 0) == std::vector<double>{48000, 96000, 192000, 384000});
    CHECK(r.listSampleRates(TX, 0) == std::vector<double>{48000});
    CHECK(r.getSampleRateRange(RX, 0).size() == 4);
    CHECK(r.getSampleRate(RX, 0) == 48000);
    r.setSampleRate(RX, 0, 192000);
    CHECK(r.getSampleRate(RX, 3) == 192000);
    // No stream: the radio stays at one receiver, 48 ksps. The command goes
    // out on the device thread.
    CHECK(wait_until([&] {
        const auto general = rig.last(0x00);
        return general && general->data == hl2::general(0, 1).data;
    }));
    r.setSampleRate(TX, 0, 48000);
    CHECK(r.getSampleRate(TX, 0) == 48000);
    CHECK_THROWS_AS(r.setSampleRate(RX, 0, 250000), std::invalid_argument);
    CHECK_THROWS_AS(r.setSampleRate(TX, 0, 96000), std::invalid_argument);
}

TEST_CASE("stream setup checks") {
    const Rig rig;
    Radio& r = *rig.radio;
    CHECK_THROWS_AS(r.setupStream(RX, "CU8", {}, {}), std::invalid_argument);
    CHECK_THROWS_AS(r.setupStream(TX, "CS32", {}, {}), std::invalid_argument);
    CHECK_THROWS_AS(r.setupStream(RX, "CF32", {1}, {}), std::invalid_argument);
    CHECK_THROWS_AS(r.setupStream(RX, "CF32", {0, 1, 2, 3, 4, 5, 6}, {}), std::invalid_argument);
    CHECK_THROWS_AS(r.setupStream(9, "CF32", {}, {}), std::invalid_argument);
    SoapySDR::Stream* s = r.setupStream(RX, "CF32", {}, {});
    CHECK_THROWS_AS(r.setupStream(RX, "CS16", {}, {}), std::runtime_error);
    CHECK(r.getStreamMTU(s) == daemon::RxBlock::kCapacity / 6);
    int not_a_stream = 0;
    CHECK_THROWS_AS(r.getStreamMTU(reinterpret_cast<SoapySDR::Stream*>(&not_a_stream)),
                    std::invalid_argument);
    CHECK(r.activateStream(s, SOAPY_SDR_HAS_TIME, 0, 0) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(r.deactivateStream(s, SOAPY_SDR_HAS_TIME, 0) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(r.deactivateStream(s, 0, 0) == 0);  // not active
    r.closeStream(s);
    SoapySDR::Stream* t = r.setupStream(TX, "CS16", {0}, {});
    CHECK(r.getStreamMTU(t) == 1024);
    r.closeStream(t);
}

TEST_CASE("receive streams") {
    Rig rig;
    Radio& r = *rig.radio;
    r.setSampleRate(RX, 0, 96000);
    SoapySDR::Stream* s = r.setupStream(RX, "CS32", {0, 1}, {});
    int flags = 0;
    long long time_ns = 0;
    std::vector<std::int32_t> a(200);
    std::vector<std::int32_t> b(200);
    void* buffs[] = {a.data(), b.data()};

    // Not active: a timeout.
    CHECK(r.readStream(s, buffs, 100, flags, time_ns, 1000) == SOAPY_SDR_TIMEOUT);

    REQUIRE(r.activateStream(s, 0, 0, 0) == 0);
    CHECK(r.activateStream(s, 0, 0, 0) == 0);  // already active
    REQUIRE(rig.sent([](const Command& c) {
        return c.address == 0x00 && c.data == hl2::general(1, 2).data && (c.flags & Command::kRun);
    }));
    CHECK(r.readStream(s, buffs, 100, flags, time_ns, 1000) == SOAPY_SDR_TIMEOUT);

    // Each frame's bytes all equal its index: the fake's pattern.
    rig.frames(150);
    int n = r.readStream(s, buffs, 100, flags, time_ns, 1'000'000);
    REQUIRE(n > 0);
    CHECK(flags == 0);
    CHECK(time_ns == 0);
    int got = n;
    while (got < 150) {
        n = r.readStream(s, buffs, 100, flags, time_ns, 1'000'000);
        REQUIRE(n > 0);
        got += n;
    }
    CHECK(got == 150);
    // The last frame read was index 149 (0x95): every value 0x959595.
    const std::int32_t v = static_cast<std::int32_t>(0xff959595u);
    CHECK(a[static_cast<std::size_t>(n - 1) * 2] == v);
    CHECK(b[static_cast<std::size_t>(n - 1) * 2 + 1] == v);

    // An overrun in the driver: one overflow, then data again.
    {
        const std::lock_guard lock(rig.fake->mutex);
        rig.fake->stats_.rx_overrun_frames += 5;
        rig.fake->overrun = true;
    }
    CHECK(r.readStream(s, buffs, 100, flags, time_ns, 1'000'000) == SOAPY_SDR_OVERFLOW);
    rig.frames(10);
    CHECK(r.readStream(s, buffs, 100, flags, time_ns, 1'000'000) > 0);

    // A rate change while streaming; blocks queued at the old rate go.
    rig.frames(50);
    REQUIRE(wait_until([&] { return rig.frames_read(); }));
    std::this_thread::sleep_for(20ms);
    r.setSampleRate(RX, 0, 192000);
    REQUIRE(rig.sent([](const Command& c) { return c.data == hl2::general(2, 2).data; }));

    CHECK(r.deactivateStream(s, 0, 0) == 0);
    REQUIRE(rig.sent([](const Command& c) {
        return c.address == 0x00 && c.data == hl2::general(0, 1).data &&
               (c.flags & Command::kRun) == 0;
    }));
    CHECK(r.readStream(s, buffs, 100, flags, time_ns, 1000) == SOAPY_SDR_TIMEOUT);
    r.closeStream(s);
}

TEST_CASE("receive capacity is checked at activation") {
    Rig rig({}, fast(), [](FakeDevice& d) { d.info_.rx_values_max = 1536000; });
    Radio& r = *rig.radio;
    r.setSampleRate(RX, 0, 384000);
    SoapySDR::Stream* s = r.setupStream(RX, "CF32", {0, 1, 2}, {});
    const test::LogCapture capture;
    CHECK(r.activateStream(s, 0, 0, 0) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(capture.contains("3 channels at 384 ksps asked for; the link carries 2"));
    r.setSampleRate(RX, 0, 192000);
    CHECK(r.activateStream(s, 0, 0, 0) == 0);
    CHECK_THROWS_AS(r.setSampleRate(RX, 0, 384000), std::invalid_argument);
    r.closeStream(s);
}

TEST_CASE("a reader that falls behind gets an overflow") {
    Rig rig;
    Radio& r = *rig.radio;
    SoapySDR::Stream* s = r.setupStream(RX, "CF32", {}, {});
    REQUIRE(r.activateStream(s, 0, 0, 0) == 0);
    REQUIRE(rig.sent([](const Command& c) { return (c.flags & Command::kRun) != 0; }));
    // More blocks than the queue holds (a block is at most 2730 frames).
    for (int i = 0; i < 80; ++i) {
        rig.frames(2730);
        REQUIRE(wait_until([&] { return rig.frames_read(); }));
    }
    std::this_thread::sleep_for(20ms);
    std::vector<std::complex<float>> a(4096);
    void* buffs[] = {a.data()};
    int flags = 0;
    long long time_ns = 0;
    CHECK(r.readStream(s, buffs, a.size(), flags, time_ns, 100000) == SOAPY_SDR_OVERFLOW);
    CHECK(r.readStream(s, buffs, a.size(), flags, time_ns, 100000) > 0);
    // Deactivated with blocks still queued: they are dropped.
    CHECK(r.deactivateStream(s, 0, 0) == 0);
    r.closeStream(s);
}

TEST_CASE("a device error ends the streams") {
    Rig rig(transmit_settings());
    Radio& r = *rig.radio;
    SoapySDR::Stream* s = r.setupStream(RX, "CF32", {}, {});
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(s, 0, 0, 0) == 0);
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    {
        const std::lock_guard lock(rig.fake->mutex);
        rig.fake->fail_on = "read";
        rig.fake->frames_ready = 10;
    }
    std::vector<std::complex<float>> a(100);
    void* buffs[] = {a.data()};
    const void* tx_buffs[] = {a.data()};
    int flags = 0;
    long long time_ns = 0;
    CHECK(r.readStream(s, buffs, a.size(), flags, time_ns, 1'000'000) == SOAPY_SDR_STREAM_ERROR);
    CHECK(r.writeStream(t, tx_buffs, a.size(), flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
    CHECK(r.readSensor("transmitting") == "false");
}

TEST_CASE("transmit needs allow_tx") {
    const Rig rig;
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    CHECK(r.activateStream(t, 0, 0, 0) == SOAPY_SDR_NOT_SUPPORTED);
    std::vector<std::complex<float>> a(10);
    const void* buffs[] = {a.data()};
    int flags = 0;
    CHECK(r.writeStream(t, buffs, a.size(), flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
    for (const Command& c : rig.commands()) {
        CHECK_FALSE(c.can_transmit());
    }
}

TEST_CASE("a burst keys the radio and ends with SOAPY_SDR_END_BURST") {
    Rig rig(transmit_settings());
    Radio& r = *rig.radio;
    r.setGain(TX, 0, 10.0);  // code 20: drive 131
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    SoapySDR::Stream* s = r.setupStream(RX, "CF32", {}, {});
    std::vector<std::complex<float>> a(600, {0.5f, -0.25f});
    a[0] = {2.0f, 0.0f};  // clipped
    const void* buffs[] = {a.data()};
    void* rx_buffs[] = {a.data()};
    int flags = 0;
    long long time_ns = 0;
    CHECK(r.writeStream(s, buffs, 1, flags, 0, 1000) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(r.readStream(t, rx_buffs, 1, flags, time_ns, 1000) == SOAPY_SDR_NOT_SUPPORTED);
    r.closeStream(s);

    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    CHECK(r.activateStream(t, 0, 0, 0) == 0);
    // Run is set for transmit too.
    REQUIRE(rig.sent([](const Command& c) { return (c.flags & Command::kRun) != 0; }));
    CHECK_FALSE(rig.radio->burst_open());

    CHECK(r.writeStream(t, buffs, 300, flags, 0, 1'000'000) == 300);
    CHECK(r.burst_open());
    CHECK(r.transmitting());
    CHECK(r.readSensor("transmitting") == "true");
    REQUIRE(rig.sent([](const Command& c) {
        return c.ptt && (c.flags & Command::kTxPermit) && (c.flags & Command::kPaAllowed);
    }));
    REQUIRE(rig.sent(
        [](const Command& c) { return c.address == 0x09 && c.data == (131u << 24 | 1u << 19); }));
    REQUIRE(wait_until([&] { return rig.written_samples() == 300; }));
    {
        const std::lock_guard lock(rig.fake->mutex);
        const auto& w = rig.fake->written;
        CHECK(w[0] == 0x00);  // Q of the clipped sample, then I at full scale
        CHECK(w[2] == 0x7f);
        CHECK(w[4] == 0xe0);  // Q = -0.25: 0xe000
        CHECK(w[6] == 0x40);  // I = 0.5: 0x4000
    }

    // A drive change while keyed goes out at once.
    r.setGain(TX, 0, 5.0);
    REQUIRE(rig.sent([](const Command& c) { return c.address == 0x09 && (c.data >> 24) == 67; }));

    flags = SOAPY_SDR_END_BURST;
    CHECK(r.writeStream(t, buffs, 300, flags, 0, 1'000'000) == 300);
    CHECK(flags == 0);
    // MOX holds until the driver's ring has emptied, then for the tail.
    std::this_thread::sleep_for(30ms);
    CHECK(r.transmitting());
    const test::LogCapture capture;
    REQUIRE(wait_until([&] {
        rig.drain();
        return !r.burst_open();
    }));
    CHECK_FALSE(r.transmitting());
    REQUIRE(rig.sent([](const Command& c) { return c.address == 0x09 && c.data == 67u << 24; }));
    CHECK_FALSE(rig.keyed_now());
    CHECK(capture.contains("2 TX samples beyond full scale, clipped"));

    CHECK(r.deactivateStream(t, 0, 0) == 0);
    REQUIRE(rig.sent([](const Command& c) { return (c.flags & Command::kRun) == 0; }));
    r.closeStream(t);
}

TEST_CASE("a pause in the writes keeps the radio keyed; only the program unkeys it") {
    Rig rig(transmit_settings());
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CS16", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::int16_t> a(200, 1000);
    const void* buffs[] = {a.data()};
    int flags = 0;
    CHECK(r.writeStream(t, buffs, 100, flags, 0, 1'000'000) == 100);
    CHECK(r.transmitting());
    // Every sample sent, and no write for a while: still keyed.
    REQUIRE(wait_until([&] { return rig.written_samples() == 100; }));
    rig.drain();
    std::this_thread::sleep_for(300ms);
    CHECK(r.burst_open());
    CHECK(r.transmitting());
    // The burst goes on with the next write.
    CHECK(r.writeStream(t, buffs, 100, flags, 0, 1'000'000) == 100);
    CHECK(r.transmitting());
    REQUIRE(wait_until([&] { return rig.written_samples() == 200; }));
    rig.drain();
    CHECK(r.deactivateStream(t, 0, 0) == 0);
    CHECK_FALSE(r.transmitting());
    CHECK_FALSE(rig.keyed_now());
    // Not active: refused.
    CHECK(r.writeStream(t, buffs, 100, flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
}

TEST_CASE("deactivation ends a burst that does not drain") {
    Rig rig(transmit_settings());
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::complex<float>> a(100);
    const void* buffs[] = {a.data()};
    int flags = 0;
    CHECK(r.writeStream(t, buffs, 100, flags, 0, 1'000'000) == 100);
    // The fake's ring never drains: after Timing::tx_drain MOX goes anyway.
    CHECK(r.deactivateStream(t, 0, 0) == 0);
    CHECK_FALSE(r.transmitting());
    REQUIRE(rig.sent([](const Command& c) { return !c.ptt; }));
    CHECK_FALSE(rig.keyed_now());
}

TEST_CASE("jack keying holds the permission bit and never PTT while the device is open") {
    Settings settings = transmit_settings();
    settings.tx.jack_keying = true;
    Rig rig(settings);
    // From the start, before any stream: permission, no PTT.
    REQUIRE(rig.sent([](const Command& c) { return (c.flags & Command::kTxPermit) != 0; }));
    for (const Command& c : rig.commands()) {
        CHECK_FALSE(c.ptt);
    }
    CHECK_FALSE(rig.radio->transmitting());
    // A burst keys and unkeys as usual; the permission stays after it.
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::complex<float>> a(100);
    const void* buffs[] = {a.data()};
    int flags = SOAPY_SDR_END_BURST;
    CHECK(r.writeStream(t, buffs, 100, flags, 0, 1'000'000) == 100);
    REQUIRE(wait_until([&] {
        rig.drain();
        return !r.burst_open();
    }));
    CHECK(wait_until([&] { return !rig.keyed_now(); }));
    CHECK((rig.commands().back().flags & Command::kTxPermit) != 0);
    r.closeStream(t);
}

TEST_CASE("without jack keying no permission bit before a burst") {
    const Rig rig(transmit_settings());
    REQUIRE(rig.sent([](const Command& c) { return c.address == 0x0a; }));
    for (const Command& c : rig.commands()) {
        CHECK((c.flags & Command::kTxPermit) == 0);
    }
}

TEST_CASE("MOX goes when the gateware has taken the last sample") {
    Timing timing = fast();
    timing.tx_tail_max = 1000ms;
    Rig rig(transmit_settings(), timing);
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::complex<float>> a(500);
    const void* buffs[] = {a.data()};
    int flags = SOAPY_SDR_END_BURST;
    CHECK(r.writeStream(t, buffs, 500, flags, 0, 1'000'000) == 500);
    REQUIRE(wait_until([&] { return rig.written_samples() == 500; }));
    // The driver's ring empties with only part taken: still keyed.
    {
        const std::lock_guard lock(rig.fake->mutex);
        rig.fake->stats_.tx_ring_words = 0;
        rig.fake->stats_.tx_words += 200;
    }
    std::this_thread::sleep_for(20ms);
    CHECK(r.transmitting());
    // The rest taken: unkeyed within the time 300 samples take (6 ms) and
    // a check, far inside Timing::tx_tail_max.
    const auto at = std::chrono::steady_clock::now();
    {
        const std::lock_guard lock(rig.fake->mutex);
        rig.fake->stats_.tx_words += 300;
    }
    REQUIRE(wait_until([&] { return !r.burst_open(); }));
    CHECK(std::chrono::steady_clock::now() - at < 100ms);
    // The unkey goes out on the device thread.
    CHECK(wait_until([&] { return !rig.keyed_now(); }));
    r.closeStream(t);
}

TEST_CASE("a gateware counter that does not move ends the burst at the ceiling") {
    Rig rig(transmit_settings(), fast(), {}, Counter::Stuck);
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::complex<float>> a(100);
    const void* buffs[] = {a.data()};
    int flags = SOAPY_SDR_END_BURST;
    const test::LogCapture capture;
    CHECK(r.writeStream(t, buffs, 100, flags, 0, 1'000'000) == 100);
    REQUIRE(wait_until([&] {
        rig.drain();
        return !r.burst_open();
    }));
    CHECK(capture.contains("the gateware did not take the last 100 TX samples"));
    // The unkey goes out on the device thread.
    CHECK(wait_until([&] { return !rig.keyed_now(); }));
    r.closeStream(t);
}

TEST_CASE("without the gateware counter MOX is held for the fixed tail") {
    // Absent; read at the burst's start only; and the module's own sysfs
    // reader, with no driver attribute in the directory it is given.
    const auto counter = GENERATE(Counter::Absent, Counter::Vanishing, Counter::Sysfs);
    Rig rig(transmit_settings(), fast(), {}, counter);
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::complex<float>> a(100);
    const void* buffs[] = {a.data()};
    int flags = SOAPY_SDR_END_BURST;
    CHECK(r.writeStream(t, buffs, 100, flags, 0, 1'000'000) == 100);
    REQUIRE(wait_until([&] {
        rig.drain();
        return !r.burst_open();
    }));
    // The unkey goes out on the device thread.
    CHECK(wait_until([&] { return !rig.keyed_now(); }));
    r.closeStream(t);
}

TEST_CASE("writes wait for room and time out") {
    Timing timing = fast();
    timing.tx_queue_samples = 300;
    Rig rig(transmit_settings(), timing);
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::complex<float>> a(1000);
    const void* buffs[] = {a.data()};
    int flags = 0;
    // 300 go; the rest wait for room until the timeout.
    CHECK(r.writeStream(t, buffs, 1000, flags, 0, 20000) == 300);
    CHECK(r.writeStream(t, buffs, 1000, flags, 0, 0) == SOAPY_SDR_TIMEOUT);
    // Room appears as the ring drains.
    std::thread drainer([&] {
        std::this_thread::sleep_for(5ms);
        rig.drain();
    });
    CHECK(r.writeStream(t, buffs, 200, flags, 0, 1'000'000) == 200);
    drainer.join();
    rig.drain();
    r.closeStream(t);
    CHECK_FALSE(r.transmitting());
}

TEST_CASE("the watchdog drops MOX until the burst ends") {
    Settings settings = transmit_settings();
    settings.tx.watchdog = 30ms;
    Rig rig(settings);
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::complex<float>> a(10);
    const void* buffs[] = {a.data()};
    int flags = 0;
    REQUIRE(r.writeStream(t, buffs, 10, flags, 0, 1'000'000) == 10);
    REQUIRE(wait_until([&] {
        rig.drain();
        return !r.transmitting();
    }));
    CHECK(r.burst_open());
    // Refused while latched; the end of the burst clears the latch.
    CHECK(r.writeStream(t, buffs, 10, flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
    flags = SOAPY_SDR_END_BURST;
    CHECK(r.writeStream(t, buffs, 10, flags, 0, 1000) == SOAPY_SDR_STREAM_ERROR);
    REQUIRE(wait_until([&] { return !r.burst_open(); }));
    flags = 0;
    CHECK(r.writeStream(t, buffs, 10, flags, 0, 1'000'000) == 10);
    CHECK(r.transmitting());
    rig.drain();
    r.closeStream(t);
}

TEST_CASE("faults are logged") {
    Rig rig(transmit_settings());
    Radio& r = *rig.radio;
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    const test::LogCapture capture;
    {
        const std::lock_guard lock(rig.fake->mutex);
        rig.fake->stats_.tx_underruns += 2;
        rig.fake->stats_.rx_resyncs += 1;
    }
    REQUIRE(wait_until([&] { return capture.contains("rx-resync: 1"); }));
    REQUIRE(wait_until([&] { return capture.contains("tx-underrun: 2"); }));
    CHECK(r.readSensor("rx_resyncs") == "1");
    CHECK(r.readSensor("tx_underruns") == "2");
    r.closeStream(t);
}

TEST_CASE("sensors") {
    const Rig rig;
    Radio& r = *rig.radio;
    const auto keys = r.listSensors();
    CHECK(keys.size() == 7);
    for (const auto& key : keys) {
        const auto info = r.getSensorInfo(key);
        CHECK(info.key == key);
        CHECK_FALSE(info.description.empty());
        CHECK_FALSE(r.readSensor(key).empty());
    }
    CHECK(r.getSensorInfo("rx_overruns").type == SoapySDR::ArgInfo::INT);
    CHECK(r.getSensorInfo("rx_stalled").type == SoapySDR::ArgInfo::BOOL);
    CHECK(r.readSensor("rx_stalled") == "false");
    CHECK(r.readSensor("ptt_input") == "false");
    CHECK(r.readSensor("cw_key") == "false");
    CHECK_THROWS_AS(r.getSensorInfo("temperature"), std::invalid_argument);
    CHECK_THROWS_AS(r.readSensor("temperature"), std::invalid_argument);
}

TEST_CASE("a radio closed with its streams active is left unkeyed") {
    Rig rig(transmit_settings());
    Radio& r = *rig.radio;
    SoapySDR::Stream* s = r.setupStream(RX, "CF32", {}, {});
    SoapySDR::Stream* t = r.setupStream(TX, "CF32", {}, {});
    REQUIRE(r.activateStream(s, 0, 0, 0) == 0);
    REQUIRE(r.activateStream(t, 0, 0, 0) == 0);
    std::vector<std::complex<float>> a(10);
    const void* buffs[] = {a.data()};
    int flags = 0;
    REQUIRE(r.writeStream(t, buffs, 10, flags, 0, 1'000'000) == 10);
    rig.drain();
    // The destructor ends the burst, stops both streams and the device
    // layer, which sends the safe command.
    rig.radio.reset();
}
