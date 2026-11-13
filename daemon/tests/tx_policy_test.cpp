// SPDX-License-Identifier: GPL-2.0-or-later
#include "tx_policy.hpp"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

using namespace radioberry;
using namespace radioberry::daemon;
using namespace std::chrono_literals;

namespace {

const Owner kClient{Protocol::P1, {0xc0000201, 50000, false}};
constexpr std::uint32_t kPa = 1u << 19;
constexpr std::uint32_t kVna = 1u << 23;
constexpr std::uint32_t kTrDisable = 1u << 18;

struct Rig {
    explicit Rig(TxPolicyOptions o) : policy(control, session, o) { session.claim(kClient, 0s); }
    Control control;
    Session session{[this] { control.safe(); }, 3s};
    TxPolicy policy;

    std::vector<Command> drain() {
        std::vector<Command> out;
        while (const auto c = control.next()) {
            out.push_back(*c);
        }
        return out;
    }
};

TxPolicyOptions allowed(bool pa = false, std::uint8_t ceiling = 0x40) {
    TxPolicyOptions o;
    o.allow_tx = true;
    o.allow_pa = pa;
    o.drive_ceiling = ceiling;
    o.watchdog = 10s;
    return o;
}

// Captures the default logger's messages for the life of the object.
class LogCapture {
public:
    LogCapture()
        : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(64)),
          previous_(spdlog::default_logger()) {
        sink_->set_pattern("%v");
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("capture", sink_));
    }
    ~LogCapture() { spdlog::set_default_logger(previous_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    // Messages since the last call.
    std::vector<std::string> take() {
        std::vector<std::string> lines = sink_->last_formatted();
        std::vector<std::string> out(lines.begin() + static_cast<std::ptrdiff_t>(seen_),
                                     lines.end());
        seen_ = lines.size();
        for (std::string& line : out) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
        }
        return out;
    }

private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> previous_;
    std::size_t seen_ = 0;
};

}  // namespace

TEST_CASE("every drive change while transmitting is logged", "[policy]") {
    LogCapture log;
    Rig rig(allowed(true, 0x40));
    // Receive value in place when MOX arrives, as from piHPSDR.
    rig.policy.set_tx_drive(0);
    rig.policy.update(kClient, false, 0s);
    log.take();  // the session's ownership message
    rig.policy.update(kClient, true, 1s);
    CHECK(log.take() == std::vector<std::string>{"PTT on: drive 0 (ceiling 64)"});
    // The client's transmit drive arrives a few packets later.
    rig.policy.set_tx_drive(0x2eu << 24);
    rig.policy.update(kClient, true, 2s);
    CHECK(log.take() == std::vector<std::string>{"drive 46 (ceiling 64)"});
    rig.policy.update(kClient, true, 3s);  // unchanged: nothing
    CHECK(log.take().empty());
    // Above the ceiling: the capped value and the request.
    rig.policy.set_tx_drive(0x90u << 24 | kPa);
    rig.policy.update(kClient, true, 4s);
    CHECK(log.take() == std::vector<std::string>{"drive 64 (client asked for 144, ceiling 64)",
                                                 "PA enabled (envpa follows PTT)"});
    rig.policy.update(kClient, false, 5s);
    CHECK(log.take() == std::vector<std::string>{"PTT off", "PA disabled"});
    // Drive changes while receiving are not logged.
    rig.policy.set_tx_drive(0x10u << 24);
    rig.policy.update(kClient, false, 6s);
    CHECK(log.take().empty());
    rig.policy.update(kClient, true, 7s);
    CHECK(log.take() == std::vector<std::string>{"PTT on: drive 16 (ceiling 64)"});
}

TEST_CASE("the defaults never transmit", "[policy]") {
    Rig rig(TxPolicyOptions{});
    CHECK_FALSE(rig.policy.options().allow_tx);
    CHECK_FALSE(rig.policy.options().allow_pa);
    CHECK(rig.policy.options().drive_ceiling == 0);
    rig.policy.set_tx_drive(0xffu << 24 | kPa | kVna);
    rig.policy.set_cwx(true);
    rig.policy.update(kClient, true, 0s);
    rig.policy.update(kClient, true, 1s);  // refusal logged once
    CHECK_FALSE(rig.policy.transmitting());
    CHECK_FALSE(rig.session.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
    const auto commands = rig.drain();
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].address == 0x09);
    CHECK(commands[0].data == 0);  // drive capped at 0, no PA, no VNA
    CHECK_FALSE(commands[0].can_transmit());
    rig.policy.update(kClient, false, 2s);
    rig.policy.update(kClient, true, 3s);  // logged again after a release
    CHECK_FALSE(rig.policy.transmitting());
}

TEST_CASE("a receive-only gateware refuses MOX even with transmit allowed", "[policy]") {
    LogCapture log;
    TxPolicyOptions o = allowed(true, 0xff);
    o.tx_available = false;
    Rig rig(o);
    rig.policy.set_tx_drive(0xffu << 24 | kPa);
    rig.policy.update(kClient, false, 0s);
    log.take();
    rig.policy.update(kClient, true, 1s);
    rig.policy.update(kClient, true, 2s);  // refusal logged once
    CHECK(log.take() == std::vector<std::string>{
                            "MOX refused: the gateware has no transmitter (receive-only image)"});
    CHECK_FALSE(rig.policy.transmitting());
    CHECK_FALSE(rig.session.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
    for (const Command& c : rig.drain()) {
        CHECK_FALSE(c.can_transmit());
    }
}

TEST_CASE("MOX keys PTT and the TX permission bit only while granted", "[policy]") {
    Rig rig(allowed());
    rig.policy.update(kClient, false, 0s);
    rig.drain();
    rig.policy.update(kClient, true, 1s);
    CHECK(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{true, true, false});
    // The change rides the next command: here the 0x09 written before.
    const auto on = rig.drain();
    REQUIRE(on.size() == 1);
    CHECK(on[0].ptt);
    CHECK((on[0].flags & Command::kTxPermit) != 0);
    rig.policy.update(kClient, true, 2s);  // still held: nothing new
    CHECK(rig.drain().empty());
    rig.policy.update(kClient, false, 3s);
    CHECK_FALSE(rig.policy.transmitting());
    const auto off = rig.drain();
    REQUIRE(off.size() == 1);
    CHECK_FALSE(off[0].can_transmit());
}

TEST_CASE("no MOX without an owner", "[policy]") {
    Rig rig(allowed());
    rig.session.release(kClient, "test");
    rig.policy.update(kClient, true, 0s);
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
}

TEST_CASE("drive is capped, VNA cleared, and PA enable follows MOX", "[policy]") {
    Rig rig(allowed(true, 0x40));
    rig.policy.set_tx_drive(0xffu << 24 | kVna | kPa | kTrDisable);
    rig.policy.update(kClient, false, 0s);
    CHECK(rig.control.value(0x09) == (0x40u << 24 | kTrDisable));
    rig.policy.update(kClient, true, 1s);
    CHECK(rig.control.value(0x09) == (0x40u << 24 | kPa | kTrDisable));
    rig.policy.update(kClient, false, 2s);
    CHECK(rig.control.value(0x09) == (0x40u << 24 | kTrDisable));
    // Below the ceiling the client's drive is sent as is.
    rig.policy.set_tx_drive(0x20u << 24);
    rig.policy.update(kClient, true, 3s);
    CHECK(rig.control.value(0x09) == 0x20u << 24);
    CHECK(rig.policy.effective_tx_drive(true) == 0x20u << 24);
}

TEST_CASE("PA enable is never sent unless the PA is allowed", "[policy]") {
    Rig rig(allowed(false));
    rig.policy.set_tx_drive(0x10u << 24 | kPa);
    rig.policy.update(kClient, true, 0s);
    CHECK(rig.policy.transmitting());
    CHECK(rig.control.value(0x09) == 0x10u << 24);
    CHECK(rig.policy.effective_tx_drive(true) == 0x10u << 24);
}

TEST_CASE("CWX is set only with MOX", "[policy]") {
    Rig rig(allowed());
    rig.policy.set_cwx(true);
    rig.policy.update(kClient, false, 0s);
    CHECK_FALSE(rig.control.transmit().cwx);
    rig.policy.update(kClient, true, 1s);
    CHECK(rig.control.transmit().cwx);
    rig.policy.set_cwx(false);
    rig.policy.update(kClient, true, 2s);
    CHECK_FALSE(rig.control.transmit().cwx);
}

TEST_CASE("the watchdog drops MOX and latches until it is released", "[policy]") {
    Rig rig(allowed(true));
    rig.policy.set_tx_drive(0x30u << 24 | kPa);
    rig.policy.tick(100s);  // not transmitting: nothing
    rig.policy.update(kClient, true, 0s);
    rig.policy.update(kClient, true, 9s);  // held: the timer runs from the grant
    rig.policy.tick(9s);
    CHECK(rig.policy.transmitting());
    rig.policy.tick(10s);
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.session.latched());
    CHECK(rig.control.transmit() == TransmitState{});
    // The safe state cleared drive and PA enable.
    CHECK(rig.control.value(0x09) == 0);
    rig.policy.update(kClient, true, 11s);
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
    CHECK((*rig.control.value(0x09) & kPa) == 0);
    rig.policy.update(kClient, false, 12s);
    CHECK_FALSE(rig.session.latched());
    rig.policy.update(kClient, true, 13s);
    CHECK(rig.policy.transmitting());
    rig.policy.tick(22s);  // the timer restarted at 13 s
    CHECK(rig.policy.transmitting());
}

TEST_CASE("a stream fault while transmitting leaves the gateware safe", "[policy]") {
    Rig rig(allowed(true));
    rig.policy.set_tx_drive(0x30u << 24 | kPa);
    rig.policy.update(kClient, true, 0s);
    rig.drain();
    rig.session.fault("TX underrun");  // from the device thread
    CHECK(rig.control.transmit() == TransmitState{});
    const auto safe = rig.drain();
    REQUIRE_FALSE(safe.empty());
    CHECK(safe[0].address == 0x09);
    CHECK(safe[0].data == 0);
    CHECK_FALSE(safe[0].can_transmit());
    // The client still asks for MOX: refused, nothing keyed.
    rig.policy.update(kClient, true, 1s);
    CHECK_FALSE(rig.policy.transmitting());
    for (const Command& c : rig.drain()) {
        CHECK_FALSE(c.can_transmit());
        CHECK((c.data & kPa) == 0);
    }
}

TEST_CASE("losing the owner while transmitting leaves the gateware safe", "[policy]") {
    Rig rig(allowed(true));
    rig.policy.set_tx_drive(0x30u << 24 | kPa);
    rig.policy.update(kClient, true, 0s);
    rig.session.tick(10s);  // silent client released
    CHECK(rig.control.transmit() == TransmitState{});
    rig.policy.update(kClient, true, 11s);  // a late packet
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
}

TEST_CASE("only the owner's MOX counts", "[policy]") {
    Rig rig(allowed());
    const Owner other{Protocol::P1, {0xc0000209, 50000, false}};
    rig.policy.update(other, true, 0s);
    CHECK_FALSE(rig.policy.transmitting());
    CHECK(rig.control.transmit() == TransmitState{});
    rig.policy.update(kClient, true, 1s);
    rig.policy.update(other, false, 2s);  // cannot release the owner's MOX
    CHECK(rig.policy.transmitting());
    CHECK(rig.control.transmit().ptt);
}
