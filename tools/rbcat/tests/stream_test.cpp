// SPDX-License-Identifier: GPL-2.0-or-later
#include "stream.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <sstream>
#include <stdexcept>

#include "fake_device.hpp"

using namespace radioberry;
using namespace radioberry::cat;
using namespace radioberry::cat::test;
using Catch::Approx;

namespace {

struct Fixture {
    FakeClock clock;
    FakeDevice device{clock};
    std::ostringstream samples;
    std::ostringstream progress;
    bool stop_now = false;

    StreamResult run(const Options& options) {
        return run_stream(device, clock, options, samples, progress, [this] { return stop_now; });
    }
};

Options timed(double seconds) {
    Options o;
    o.seconds = seconds;
    return o;
}

}  // namespace

TEST_CASE("a device without its own wait_or_wake waits as wait() does", "[stream]") {
    FakeClock clock;
    FakeDevice device(clock);
    Device& d = device;
    const Readiness r = d.wait_or_wake(true, true, 0, -1);
    CHECK_FALSE(r.readable);
    CHECK_FALSE(r.writable);
    CHECK_FALSE(r.woken);
}

TEST_CASE("configures the receivers with receive-only commands", "[stream]") {
    Fixture fx;
    Options o = timed(0.1);
    o.rate_ksps = 192;
    o.rate_code = 2;
    o.receivers = 3;
    o.frequencies_hz = {7074000, 10136000, 14074000};
    o.lna_gain_db = 10;
    const StreamResult r = fx.run(o);

    REQUIRE(fx.device.commands.size() == 5);
    CHECK(fx.device.commands[0] == hl2::general(2, 3));
    CHECK(fx.device.commands[1] == hl2::rx_frequency(0, 7074000));
    CHECK(fx.device.commands[2] == hl2::rx_frequency(1, 10136000));
    CHECK(fx.device.commands[3] == hl2::rx_frequency(2, 14074000));
    CHECK(fx.device.commands[4] == hl2::lna_gain(10));
    for (const Command& c : fx.device.commands) {
        CHECK_FALSE(c.ptt);
        CHECK((c.flags & Command::kTxPermit) == 0);
        CHECK_FALSE(c.can_transmit());
    }
    CHECK(r.commands == fx.device.commands);
    CHECK(r.info.active_receivers == 3);
    CHECK(r.info.rx_rate_code == 2);
}

TEST_CASE("measures frames and level for the requested time", "[stream]") {
    Fixture fx;
    const StreamResult r = fx.run(timed(2.0));
    CHECK(r.seconds == Approx(2.0).margin(0.002));
    CHECK(r.frames == Approx(96000).margin(48));
    CHECK_FALSE(r.interrupted);
    CHECK(r.bytes_out == 0);
    CHECK(fx.samples.str().empty());
    REQUIRE(r.levels.size() == 1);
    // I at half scale, Q zero: 6.02 dB below a full-scale complex tone.
    CHECK(*r.levels[0].rms_dbfs() == Approx(-6.0206).margin(0.001));
    CHECK(*r.levels[0].peak_dbfs() == Approx(-6.0206).margin(0.001));
    CHECK(r.end.rx_frames - r.measure_start.rx_frames >= r.frames);
}

TEST_CASE("negative full scale and silence", "[stream]") {
    Fixture fx;
    fx.device.i_value = -8388608;
    fx.device.q_value = 0;
    StreamResult r = fx.run(timed(0.01));
    CHECK(r.levels[0].peak == 8388608);
    CHECK(*r.levels[0].peak_dbfs() == Approx(0.0).margin(1e-9));

    Fixture quiet;
    quiet.device.i_value = 0;
    r = quiet.run(timed(0.01));
    CHECK_FALSE(r.levels[0].rms_dbfs().has_value());
    CHECK_FALSE(r.levels[0].peak_dbfs().has_value());
    CHECK_FALSE(Level{}.rms_dbfs().has_value());
}

TEST_CASE("iq24 output is the frames as read", "[stream]") {
    Fixture fx;
    fx.device.i_value = 0x123456;
    fx.device.q_value = -2;
    Options o = timed(0.01);
    o.output = Output::kIq24;
    o.receivers = 2;
    o.frequencies_hz = {1, 2};
    const StreamResult r = fx.run(o);
    const std::string out = fx.samples.str();
    CHECK(r.bytes_out == out.size());
    CHECK(out.size() == r.frames * 12);
    REQUIRE(out.size() >= 12);
    const std::string frame("\x12\x34\x56\xff\xff\xfe\x12\x34\x56\xff\xff\xfe", 12);
    CHECK(out.substr(0, 12) == frame);
    CHECK(r.levels.size() == 2);
}

TEST_CASE("iq16 output is the top 16 bits, little-endian", "[stream]") {
    Fixture fx;
    fx.device.i_value = 0x123456;
    fx.device.q_value = -2;
    Options o = timed(0.01);
    o.output = Output::kIq16;
    const StreamResult r = fx.run(o);
    const std::string out = fx.samples.str();
    CHECK(r.bytes_out == out.size());
    CHECK(out.size() == r.frames * 4);
    REQUIRE(out.size() >= 4);
    CHECK(out.substr(0, 4) == std::string("\x34\x12\xff\xff", 4));
}

TEST_CASE("faults while settling are kept apart from the measurement", "[stream]") {
    Fixture fx;
    fx.device.on_read = [&](FakeDevice& d) {
        if (fx.clock.t < 1000.2) {
            d.stats_.rx_resyncs += 1;
            d.stats_.rx_dropped_words += 7;
        }
    };
    const StreamResult r = fx.run(timed(1.0));
    CHECK(r.measure_start.rx_resyncs - r.before.rx_resyncs > 0);
    CHECK(r.end.rx_resyncs == r.measure_start.rx_resyncs);
    CHECK(r.end.rx_dropped_words == r.measure_start.rx_dropped_words);
}

TEST_CASE("faults while measuring show in the end statistics", "[stream]") {
    Fixture fx;
    fx.device.on_read = [](FakeDevice& d) {
        if (d.reads == 700) {
            d.stats_.rx_overrun_frames += 3;
        }
    };
    const StreamResult r = fx.run(timed(1.0));
    CHECK(r.end.rx_overrun_frames - r.measure_start.rx_overrun_frames == 3);
}

TEST_CASE("stop ends the stream while settling or measuring", "[stream]") {
    Fixture settling;
    settling.stop_now = true;
    StreamResult r = settling.run(timed(1.0));
    CHECK(r.interrupted);
    CHECK(r.frames == 0);
    CHECK(settling.device.reads == 0);

    Fixture measuring;
    measuring.device.on_read = [&](FakeDevice& d) {
        if (d.reads == 600) {
            measuring.stop_now = true;
        }
    };
    r = measuring.run(timed(0));  // until stopped
    CHECK(r.interrupted);
    CHECK(r.seconds == Approx(0.1).margin(0.002));
}

TEST_CASE("interrupted reads deliver nothing and the stream carries on", "[stream]") {
    Fixture fx;
    Options o = timed(0.01);
    o.settle_seconds = 0;
    fx.device.interrupted_reads = 3;
    const StreamResult r = fx.run(o);
    CHECK(r.frames == Approx(480).margin(48));
    CHECK(fx.device.reads >= 13);
}

TEST_CASE("progress lines at the interval", "[stream]") {
    Fixture fx;
    Options o = timed(3.0);
    o.interval_seconds = 1.0;
    fx.device.on_read = [](FakeDevice& d) {
        if (d.reads == 1600) {
            d.stats_.rx_resyncs += 2;
        }
    };
    fx.run(o);
    const std::string p = fx.progress.str();
    CHECK(p.find("rbcat: 1 s, 48000 frames/s, resyncs 0, dropped words 0, overruns 0\n") == 0);
    CHECK(p.find("rbcat: 2 s, 48000 frames/s, resyncs 2,") != std::string::npos);
}

TEST_CASE("refuses what the gateware or driver cannot do", "[stream]") {
    Fixture fewer;
    fewer.device.info_.receivers = 4;
    Options o = timed(0.1);
    o.receivers = 6;
    o.frequencies_hz.assign(6, 14074000);
    CHECK_THROWS_WITH(fewer.run(o), "the gateware has 4 receivers; 6 requested");
    CHECK(fewer.device.commands.empty());

    Fixture ignored;
    ignored.device.follow_commands = false;
    o.receivers = 2;
    o.frequencies_hz.assign(2, 14074000);
    CHECK_THROWS_WITH(ignored.run(o),
                      "the driver reports 1 receivers at rate code 0 after configuring 2 at rate "
                      "code 0");

    Fixture rate;
    Options fast = timed(0.1);
    fast.rate_ksps = 96;
    fast.rate_code = 1;
    rate.device.follow_commands = false;
    CHECK_THROWS_AS(rate.run(fast), std::runtime_error);
}

TEST_CASE("a failing sample sink stops the stream", "[stream]") {
    Fixture fx;
    fx.samples.setstate(std::ios::badbit);
    Options o = timed(1.0);
    o.output = Output::kIq24;
    CHECK_THROWS_WITH(fx.run(o), "writing samples failed");
}

TEST_CASE("commands that could transmit are refused", "[stream]") {
    FakeClock clock;
    FakeDevice device(clock);
    Command ptt;
    ptt.ptt = true;
    CHECK_THROWS_AS(send_receive_only(device, ptt), std::logic_error);
    Command permit;
    permit.flags = Command::kRun | Command::kTxPermit;
    CHECK_THROWS_AS(send_receive_only(device, permit), std::logic_error);
    Command cwx;
    cwx.flags = Command::kCwx;
    CHECK_THROWS_AS(send_receive_only(device, cwx), std::logic_error);
    Command drive;
    drive.address = 0x09;
    drive.data = 0x10000000;
    CHECK_THROWS_AS(send_receive_only(device, drive), std::logic_error);
    Command hang;
    hang.address = 0x17;
    hang.data = 31u << 8;
    CHECK_THROWS_AS(send_receive_only(device, hang), std::logic_error);
    CHECK(device.commands.empty());
    const Reply reply = send_receive_only(device, hl2::lna_gain(0));
    CHECK(reply.version_major == 75);
    CHECK(device.commands.size() == 1);
}

TEST_CASE("tx zeros keep the driver's ring topped up at 48 ksps", "[stream][tx]") {
    Fixture fx;
    Options o = timed(2.0);
    o.tx_zeros = true;
    const StreamResult r = fx.run(o);
    CHECK_FALSE(fx.device.tx_nonzero_written);
    CHECK(r.tx_bytes == fx.device.tx_bytes_written);
    const auto words = r.end.tx_words - r.measure_start.tx_words;
    CHECK(static_cast<double>(words) == Approx(96000).margin(100));
    REQUIRE(r.tx_ring_min_words.has_value());
    CHECK(*r.tx_ring_min_words > kTxTargetWords - 100);
    CHECK(fx.device.stats_.tx_ring_words <= kTxTargetWords);
    for (const Command& c : fx.device.commands) {
        CHECK_FALSE(c.can_transmit());
    }
}

TEST_CASE("without tx zeros nothing is written", "[stream][tx]") {
    Fixture fx;
    const StreamResult r = fx.run(timed(0.1));
    CHECK(fx.device.tx_bytes_written == 0);
    CHECK(r.tx_bytes == 0);
    CHECK_FALSE(r.tx_ring_min_words.has_value());
}

TEST_CASE("tx top-up survives short and interrupted writes", "[stream][tx]") {
    Fixture fx;
    fx.device.tx_write_limit = 1024;
    fx.device.interrupted_writes = 2;
    Options o = timed(1.0);
    o.tx_zeros = true;
    const StreamResult r = fx.run(o);
    CHECK(static_cast<double>(r.end.tx_words - r.measure_start.tx_words) ==
          Approx(48000).margin(100));
    CHECK(*r.tx_ring_min_words > 0);
}

TEST_CASE("a TX ring the device does not drain stays full", "[stream][tx]") {
    Fixture fx;
    fx.device.tx_rate_scale = 0;
    Options o = timed(0.5);
    o.tx_zeros = true;
    const StreamResult r = fx.run(o);
    CHECK(r.end.tx_words == 0);
    CHECK(fx.device.tx_bytes_written == kTxTargetWords * kTxSampleBytes);
    CHECK(*r.tx_ring_min_words == kTxTargetWords);
}

TEST_CASE("tx progress lines", "[stream][tx]") {
    Fixture fx;
    Options o = timed(1.5);
    o.tx_zeros = true;
    o.interval_seconds = 1.0;
    fx.run(o);
    CHECK(fx.progress.str().find(", tx words 4") != std::string::npos);
    CHECK(fx.progress.str().find(", tx underruns 0\n") != std::string::npos);
}

TEST_CASE("a reply showing cw_on or external PTT is noted", "[stream]") {
    struct Keyed final : Device {
        explicit Keyed(FakeDevice& fake) : d(fake) {}
        radioberry_info info() override { return d.info(); }
        radioberry_stats stats() override { return d.stats(); }
        Reply command(const Command& c) override {
            Reply r = d.command(c);
            r.cw_or_ptt = d.commands.size() == 2;
            return r;
        }
        std::size_t read(std::span<std::uint8_t> b) override { return d.read(b); }
        std::size_t write(std::span<const std::uint8_t> b) override { return d.write(b); }
        Readiness wait(bool r, bool w, int t) override { return d.wait(r, w, t); }
        FakeDevice& d;
    };
    Fixture fx;
    Keyed keyed(fx.device);
    const StreamResult r =
        run_stream(keyed, fx.clock, timed(0.01), fx.samples, fx.progress, [] { return false; });
    CHECK(r.reply_tx_indicated);
    CHECK_FALSE(fx.run(timed(0.01)).reply_tx_indicated);
}
