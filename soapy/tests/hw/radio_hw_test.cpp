// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests of the SoapySDR module through SoapySDR, on the Raspberry Pi with
// the RadioBerry attached and nothing else holding /dev/radioberry. The
// module is the one SoapySDR finds: the installed package, or a build
// tree's through SOAPY_SDR_PLUGIN_PATH.
//
// The tests that run by default never key: each uses its own config file
// with allow_tx false. The [loopback] tests key the radio in the AD9866's
// digital loopback, where the image holds the PA lines low; they are
// hidden, need root for sysfs tx_loopback, allow no PA, and do not key
// unless the image reports loopback on and the PA not allowed.
//
//   rbsoapytest [Catch2 options]
//   rbsoapytest "[loopback]"
//   RADIOBERRY_SOAPY_ARGS="driver=radioberry,device=/dev/other" rbsoapytest
//   RADIOBERRY_SOAPY_SECONDS=60 rbsoapytest "[rate]"
#include <SoapySDR/Constants.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Sample = std::complex<float>;

constexpr const char* kSysfs = "/sys/bus/spi/devices/spi0.0/";
constexpr std::uint64_t kControlPaAllowed = 1u << 2;
constexpr std::uint64_t kControlLoopback = 1u << 3;
constexpr std::uint64_t kKeyed = 3;  // PTT and the TX permission bit

constexpr double kTxRate = 48000;
constexpr double kTestFrequency = 14120000;
constexpr double kTone = 1000;
constexpr std::size_t kTxBlock = 1024;

constexpr const char* kReceiveOnly = "allow_tx: false\nallow_pa: false\n";
constexpr const char* kLoopbackTx =
    "allow_tx: true\nallow_pa: false\ndrive_ceiling: 0\ntx_watchdog_s: 10\n";

std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return value != nullptr ? value : fallback;
}

double seconds_per_check() { return std::stod(env_or("RADIOBERRY_SOAPY_SECONDS", "5")); }

std::string sysfs(const std::string& name) {
    std::ifstream in(kSysfs + name);
    std::string value;
    std::getline(in, value);
    return value;
}

bool sysfs_write(const std::string& name, const std::string& value) {
    std::ofstream out(kSysfs + name);
    out << value << '\n';
    out.flush();
    return out.good();
}

// A value of sysfs `counters`: lines of "name 0xhex decimal".
std::uint64_t counter(const std::string& name) {
    std::ifstream in(std::string(kSysfs) + "counters");
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        std::string key;
        std::string hex;
        std::uint64_t value = 0;
        if (fields >> key >> hex >> value && key == name) {
            return value;
        }
    }
    FAIL("sysfs counters has no " << name);
    return 0;
}

// A config file for one test, removed with it.
class Config {
public:
    explicit Config(const char* text)
        : path_(std::filesystem::temp_directory_path() /
                ("rbsoapytest-" + std::to_string(::getpid()) + ".yaml")) {
        std::ofstream(path_) << text;
    }
    ~Config() { std::filesystem::remove(path_); }
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    std::string args() const {
        return env_or("RADIOBERRY_SOAPY_ARGS", "driver=radioberry") + ",config=" + path_.string();
    }

private:
    std::filesystem::path path_;
};

class Radio {
public:
    explicit Radio(const Config& config) : device_(SoapySDR::Device::make(config.args())) {}
    ~Radio() { SoapySDR::Device::unmake(device_); }
    Radio(const Radio&) = delete;
    Radio& operator=(const Radio&) = delete;
    SoapySDR::Device* operator->() const { return device_; }

private:
    SoapySDR::Device* device_;
};

std::vector<std::size_t> first_channels(std::size_t n) {
    std::vector<std::size_t> channels(n);
    std::iota(channels.begin(), channels.end(), std::size_t{0});
    return channels;
}

// Context switches and CPU ticks of this process's threads, without the
// calling thread.
struct ThreadCost {
    std::uint64_t switches = 0;
    std::uint64_t ticks = 0;
    unsigned threads = 0;
};

ThreadCost other_threads() {
    ThreadCost cost;
    const std::string self = std::to_string(::gettid());
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
        if (task.path().filename() == self) {
            continue;
        }
        ++cost.threads;
        std::ifstream status(task.path() / "status");
        for (std::string line; std::getline(status, line);) {
            if (line.find("ctxt_switches:") != std::string::npos) {
                cost.switches += std::stoull(line.substr(line.find(':') + 1));
            }
        }
        std::ifstream stat(task.path() / "stat");
        std::string text;
        std::getline(stat, text);
        // After the command name: state is field 3, utime 14, stime 15.
        std::istringstream fields(text.substr(text.rfind(')') + 1));
        std::string field;
        for (int i = 3; i <= 15 && fields >> field; ++i) {
            if (i >= 14) {
                cost.ticks += std::stoull(field);
            }
        }
    }
    return cost;
}

// The level in dB relative to full scale of the component of `x` at
// `frequency`, through a Hann window.
double level_db(const std::vector<Sample>& x, double frequency, double rate) {
    std::complex<double> sum = 0;
    double window_sum = 0;
    const auto n = static_cast<double>(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const auto t = static_cast<double>(i);
        const double w = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * t / n);
        sum += w * std::complex<double>(x[i]) *
               std::polar(1.0, -2 * std::numbers::pi * frequency * t / rate);
        window_sum += w;
    }
    return 20 * std::log10(std::abs(sum) / window_sum + 1e-30);
}

std::vector<Sample> tone_block(std::size_t first, std::size_t count) {
    std::vector<Sample> block(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double phase =
            2 * std::numbers::pi * kTone * static_cast<double>(first + i) / kTxRate;
        block[i] = Sample(std::polar(0.8, phase));
    }
    return block;
}

// AD9866 digital loopback for one test; off again after it.
class Loopback {
public:
    Loopback() {
        INFO("sysfs tx_loopback needs root");
        REQUIRE(sysfs_write("tx_loopback", "1"));
        REQUIRE(sysfs("tx_loopback") == "1");
    }
    ~Loopback() { sysfs_write("tx_loopback", "0"); }
    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;
};

// True when the image has loopback on and the PA not allowed: the state in
// which keying it radiates nothing.
bool safe_to_key() {
    return (counter("control") & (kControlLoopback | kControlPaAllowed)) == kControlLoopback;
}

// The child of the kill test: keys in loopback and writes the tone until it
// is killed. No Catch2 here; the exit code says what went wrong.
[[noreturn]] void transmit_until_killed(const Config& config) {
    try {
        Radio radio(config);
        radio->setFrequency(SOAPY_SDR_RX, 0, kTestFrequency);
        radio->setFrequency(SOAPY_SDR_TX, 0, kTestFrequency);
        auto* rx = radio->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0});
        auto* tx = radio->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {0});
        if (radio->activateStream(rx) != 0 || radio->activateStream(tx) != 0) {
            ::_exit(2);
        }
        if (!safe_to_key()) {
            ::_exit(3);
        }
        for (std::size_t n = 0; n < static_cast<std::size_t>(5 * kTxRate); n += kTxBlock) {
            const auto block = tone_block(n, kTxBlock);
            const void* buffers[] = {block.data()};
            int flags = 0;
            if (radio->writeStream(tx, buffers, kTxBlock, flags, 0, 1000000) < 0) {
                ::_exit(4);
            }
        }
        ::_exit(5);
    } catch (...) {
        ::_exit(6);
    }
}

}  // namespace

TEST_CASE("SoapySDR finds the radio and it identifies itself", "[find]") {
    const auto found = SoapySDR::Device::enumerate(std::string("driver=radioberry"));
    REQUIRE(found.size() == 1);
    CHECK(found[0].at("driver") == "radioberry");
    CHECK(found[0].count("label") == 1);

    const Config config(kReceiveOnly);
    const Radio radio(config);
    CHECK(radio->getDriverKey() == "radioberry");
    const auto info = radio->getHardwareInfo();
    CHECK(info.at("gateware_version") == sysfs("gateware_version"));
    CHECK(info.at("transport") == sysfs("transport"));
    CHECK(info.at("receivers") == sysfs("receivers"));
    CHECK(info.at("transmit") == "not allowed");
    CHECK(std::to_string(radio->getNumChannels(SOAPY_SDR_RX)) == sysfs("receivers"));
    CHECK(std::to_string(radio->getNumChannels(SOAPY_SDR_TX)) == sysfs("transmitters"));
}

TEST_CASE("every rate holds at the most channels the link carries", "[rate]") {
    const Config config(kReceiveOnly);
    const Radio radio(config);
    const std::size_t receivers = radio->getNumChannels(SOAPY_SDR_RX);
    const auto duration = std::chrono::duration<double>(seconds_per_check());

    for (const double rate : radio->listSampleRates(SOAPY_SDR_RX, 0)) {
        radio->setSampleRate(SOAPY_SDR_RX, 0, rate);

        // The most channels the module activates at this rate; its refusals
        // of more are errors in its log.
        SoapySDR::Stream* stream = nullptr;
        std::size_t channels = receivers;
        SoapySDR::setLogLevel(SOAPY_SDR_CRITICAL);
        for (; channels >= 1; --channels) {
            stream = radio->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, first_channels(channels));
            if (radio->activateStream(stream) == 0) {
                break;
            }
            radio->closeStream(stream);
            stream = nullptr;
        }
        SoapySDR::setLogLevel(SOAPY_SDR_INFO);
        REQUIRE(stream != nullptr);

        const std::size_t mtu = radio->getStreamMTU(stream);
        std::vector<std::vector<Sample>> data(channels, std::vector<Sample>(mtu));
        std::vector<void*> buffers;
        for (auto& channel : data) {
            buffers.push_back(channel.data());
        }
        const std::string overruns = radio->readSensor("rx_overruns");
        const std::string resyncs = radio->readSensor("rx_resyncs");

        std::uint64_t samples = 0;
        unsigned overflows = 0;
        unsigned timeouts = 0;
        unsigned errors = 0;
        std::vector<bool> alive(channels, false);
        // Timed from the first block, so the start-up is not in the rate.
        Clock::time_point first{};
        Clock::time_point last{};
        for (bool started = false; !started || last - first < duration;) {
            int flags = 0;
            long long time_ns = 0;
            const int got = radio->readStream(stream, buffers.data(), mtu, flags, time_ns, 1000000);
            last = Clock::now();
            if (got == SOAPY_SDR_OVERFLOW) {
                ++overflows;
            } else if (got == SOAPY_SDR_TIMEOUT) {
                ++timeouts;
            } else if (got < 0) {
                ++errors;
            } else if (!started) {
                started = true;
                first = last;
            } else {
                samples += static_cast<std::uint64_t>(got);
                for (std::size_t c = 0; c < channels; ++c) {
                    for (std::size_t i = 0; i < static_cast<std::size_t>(got); ++i) {
                        alive[c] = alive[c] || data[c][i] != Sample{};
                    }
                }
            }
            if (timeouts + errors > 5) {
                break;
            }
        }
        radio->deactivateStream(stream);
        radio->closeStream(stream);

        const double measured =
            static_cast<double>(samples) / std::chrono::duration<double>(last - first).count();
        std::printf(
            "%6.0f samples/s on %zu channel(s): %.1f samples/s over %.1f s, "
            "%u overflows, %u timeouts\n",
            rate, channels, measured, std::chrono::duration<double>(last - first).count(),
            overflows, timeouts);
        CAPTURE(rate, channels);
        CHECK(overflows == 0);
        CHECK(timeouts == 0);
        CHECK(errors == 0);
        CHECK(std::abs(measured - rate) < rate * 0.01);
        CHECK(radio->readSensor("rx_overruns") == overruns);
        CHECK(radio->readSensor("rx_resyncs") == resyncs);
        for (std::size_t c = 0; c < channels; ++c) {
            CAPTURE(c);
            CHECK(alive[c]);
        }
    }
    CHECK(radio->readSensor("rx_stalled") == "false");
}

TEST_CASE("an open device with no stream costs nothing", "[idle]") {
    const Config config(kReceiveOnly);
    const Radio radio(config);
    std::this_thread::sleep_for(1s);
    const ThreadCost before = other_threads();
    const std::uint64_t words = counter("words_to_pi");
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds_per_check()));
    const ThreadCost after = other_threads();

    std::printf("%u module thread(s): %llu context switches, %llu CPU ticks in %.0f s\n",
                after.threads, static_cast<unsigned long long>(after.switches - before.switches),
                static_cast<unsigned long long>(after.ticks - before.ticks), seconds_per_check());
    CHECK(after.threads == before.threads);
    CHECK(after.switches == before.switches);
    CHECK(after.ticks == before.ticks);
    CHECK(counter("words_to_pi") == words);
}

TEST_CASE("transmit is refused when the config does not allow it", "[refuse]") {
    const Config config(kReceiveOnly);
    const Radio radio(config);
    const std::uint64_t taken = counter("tx_samples");

    radio->setFrequency(SOAPY_SDR_TX, 0, kTestFrequency);
    radio->setGain(SOAPY_SDR_TX, 0, "DRIVE", 5);
    CHECK(radio->getGain(SOAPY_SDR_TX, 0, "DRIVE") == 0);
    CHECK(radio->getGainRange(SOAPY_SDR_TX, 0, "DRIVE").maximum() == 0);

    auto* rx = radio->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0});
    REQUIRE(radio->activateStream(rx) == 0);
    auto* tx = radio->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {0});
    SoapySDR::setLogLevel(SOAPY_SDR_CRITICAL);
    CHECK(radio->activateStream(tx) == SOAPY_SDR_NOT_SUPPORTED);
    SoapySDR::setLogLevel(SOAPY_SDR_INFO);
    CHECK(counter("keying") == 0);

    for (std::size_t n = 0; n < 50 * kTxBlock; n += kTxBlock) {
        const auto block = tone_block(n, kTxBlock);
        const void* buffers[] = {block.data()};
        int flags = n + kTxBlock == 50 * kTxBlock ? SOAPY_SDR_END_BURST : 0;
        CHECK(radio->writeStream(tx, buffers, kTxBlock, flags, 0, 100000) < 0);
        CHECK(radio->readSensor("transmitting") == "false");
        CHECK(counter("keying") == 0);
    }
    radio->deactivateStream(tx);
    radio->closeStream(tx);
    radio->deactivateStream(rx);
    radio->closeStream(rx);
    CHECK(counter("keying") == 0);
    CHECK(counter("tx_samples") == taken);
}

TEST_CASE("a burst in loopback comes back as its tone and unkeys", "[.][loopback]") {
    const Loopback loopback;
    const Config config(kLoopbackTx);
    const Radio radio(config);
    radio->setSampleRate(SOAPY_SDR_RX, 0, kTxRate);
    radio->setFrequency(SOAPY_SDR_RX, 0, kTestFrequency);
    radio->setFrequency(SOAPY_SDR_TX, 0, kTestFrequency);
    radio->setGain(SOAPY_SDR_TX, 0, "DRIVE", 0);

    auto* rx = radio->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0});
    auto* tx = radio->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {0});
    REQUIRE(radio->activateStream(rx) == 0);
    REQUIRE(radio->activateStream(tx) == 0);
    REQUIRE(safe_to_key());

    const std::uint64_t taken = counter("tx_samples");
    const std::uint64_t gaps = counter("tx_seq_gaps");
    const std::uint64_t bad = counter("tx_bad_frames");
    const std::uint64_t image_overflows = counter("tx_overflows");
    const std::uint64_t image_underruns = counter("tx_underruns");

    std::vector<Sample> received;
    std::vector<Sample> block(4096);
    unsigned overflow_reads = 0;
    const auto receive = [&] {
        for (;;) {
            void* buffers[] = {block.data()};
            int flags = 0;
            long long time_ns = 0;
            const int got = radio->readStream(rx, buffers, block.size(), flags, time_ns, 0);
            if (got > 0) {
                received.insert(received.end(), block.begin(), block.begin() + got);
            } else if (got == SOAPY_SDR_OVERFLOW) {
                ++overflow_reads;
            } else {
                return;
            }
        }
    };

    // Two seconds of the tone, the last block ending the burst.
    const std::size_t total = 94 * kTxBlock;
    std::uint64_t keying_mid = 0;
    std::string transmitting_mid;
    std::string underruns_early;
    const std::string underruns_before = radio->readSensor("tx_underruns");
    for (std::size_t n = 0; n < total; n += kTxBlock) {
        const auto samples = tone_block(n, kTxBlock);
        const void* buffers[] = {samples.data()};
        int flags = n + kTxBlock == total ? SOAPY_SDR_END_BURST : 0;
        REQUIRE(radio->writeStream(tx, buffers, kTxBlock, flags, 0, 1000000) ==
                static_cast<int>(kTxBlock));
        receive();
        if (n == 16 * kTxBlock) {
            underruns_early = radio->readSensor("tx_underruns");
        }
        if (n == total / 2) {
            keying_mid = counter("keying");
            transmitting_mid = radio->readSensor("transmitting");
        }
    }
    const auto written = Clock::now();
    const std::string underruns_written = radio->readSensor("tx_underruns");
    while (radio->readSensor("transmitting") == "true" && Clock::now() - written < 1s) {
        receive();
        std::this_thread::sleep_for(1ms);
    }
    const auto unkeyed_after = std::chrono::duration<double>(Clock::now() - written).count();
    const std::uint64_t keying_after = counter("keying");
    receive();
    const std::string underruns_after = radio->readSensor("tx_underruns");
    radio->deactivateStream(tx);
    radio->closeStream(tx);
    radio->deactivateStream(rx);
    radio->closeStream(rx);

    // One second from the middle of the burst.
    REQUIRE(received.size() >= static_cast<std::size_t>(1.5 * kTxRate));
    const auto middle = received.begin() + static_cast<std::ptrdiff_t>(0.5 * kTxRate);
    const std::vector<Sample> second(middle, middle + static_cast<std::ptrdiff_t>(kTxRate));
    const double tone = level_db(second, kTone, kTxRate);
    const double image = level_db(second, -kTone, kTxRate);
    std::printf(
        "loopback: tone %.1f dBFS, image %.1f dBFS, unkeyed %.3f s after the last "
        "write, image took %llu of %zu samples; driver TX underruns %s before, %s a third of a "
        "second in, %s at the last write, %s unkeyed\n",
        tone, image, unkeyed_after, static_cast<unsigned long long>(counter("tx_samples") - taken),
        total, underruns_before.c_str(), underruns_early.c_str(), underruns_written.c_str(),
        underruns_after.c_str());

    CHECK(keying_mid == kKeyed);
    CHECK(transmitting_mid == "true");
    CHECK(keying_after == 0);
    CHECK(unkeyed_after < 0.5);
    CHECK(tone > -30);
    CHECK(image < tone - 90);
    CHECK(overflow_reads == 0);
    // The driver counts each period that found its ring short with PTT set:
    // none while the burst is being written.
    CHECK(underruns_written == underruns_early);
    CHECK(counter("tx_samples") - taken == total);
    CHECK(counter("tx_seq_gaps") == gaps);
    CHECK(counter("tx_bad_frames") == bad);
    CHECK(counter("tx_overflows") == image_overflows);
    CHECK(counter("tx_underruns") == image_underruns);
    CHECK(counter("keying") == 0);
}

TEST_CASE("a program killed while keyed leaves the radio unkeyed", "[.][loopback]") {
    const Loopback loopback;
    const Config config(kLoopbackTx);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        transmit_until_killed(config);
    }

    bool keyed = false;
    for (const auto end = Clock::now() + 3s; !keyed && Clock::now() < end;) {
        std::this_thread::sleep_for(20ms);
        keyed = counter("keying") == kKeyed;
    }
    const bool safe = safe_to_key();
    ::kill(child, SIGKILL);
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CAPTURE(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    CHECK(keyed);
    CHECK(safe);
    CHECK(WIFSIGNALED(status));

    bool unkeyed = false;
    const auto killed = Clock::now();
    for (const auto end = killed + 1s; !unkeyed && Clock::now() < end;) {
        unkeyed = counter("keying") == 0;
    }
    std::printf("killed while keyed: keying 0 within %.3f s\n",
                std::chrono::duration<double>(Clock::now() - killed).count());
    CHECK(unkeyed);
}
