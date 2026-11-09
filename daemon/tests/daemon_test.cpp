// SPDX-License-Identifier: GPL-2.0-or-later
//
// radioberryd end to end over loopback sockets, against the fake device.
#include "daemon.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "fake_device.hpp"
#include "p1_packets.hpp"
#include "p2_packets.hpp"

using namespace radioberry;
using namespace radioberry::daemon;
using namespace radioberry::daemon::test;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kLoopback = 0x7f000001;

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(kLoopback);
    a.sin_port = htons(port);
    return a;
}

void timeout(int fd, int ms) {
    const timeval t{ms / 1000, (ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
}

// A client socket with a receive timeout.
struct Socket {
    explicit Socket(int type) : fd(::socket(AF_INET, type, 0)) { timeout(fd, 2000); }
    ~Socket() { ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int fd;
};

std::vector<std::uint8_t> receive(int fd, std::size_t max = 2048) {
    std::vector<std::uint8_t> b(max);
    const ssize_t n = ::recv(fd, b.data(), b.size(), 0);
    b.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return b;
}

// Reads exactly `n` bytes from a TCP stream, or fewer at its end.
std::vector<std::uint8_t> receive_all(int fd, std::size_t n) {
    std::vector<std::uint8_t> b(n);
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, b.data() + got, n - got, 0);
        if (r <= 0) {
            break;
        }
        got += static_cast<std::size_t>(r);
    }
    b.resize(got);
    return b;
}

struct Running {
    explicit Running(FakeDevice& device, DaemonOptions o = options()) : daemon(device, o) {
        thread = std::thread([this] { result = daemon.run(stop); });
    }
    ~Running() { finish(); }
    bool finish() {
        if (thread.joinable()) {
            stop = true;
            thread.join();
        }
        return result;
    }
    static DaemonOptions options() {
        DaemonOptions o;
        o.address = kLoopback;
        o.port = 0;
        o.mac = std::array<std::uint8_t, 6>{2, 0, 0, 0, 0, 9};
        o.device.wait_ms = 2;
        o.device.stall_after = 10s;
        o.realtime = false;  // one test below turns it on
        return o;
    }
    Daemon daemon;
    std::atomic<bool> stop{false};
    bool result = false;
    std::thread thread;
};

void feed(FakeDevice& device, std::size_t frames) {
    const std::lock_guard lock(device.mutex);
    device.frames_ready += frames;
}

}  // namespace

TEST_CASE("radioberryd serves discovery and a UDP stream", "[daemon]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    Running r(device);
    CHECK(r.daemon.identity().version_major == 75);
    CHECK(r.daemon.identity().receivers == 6);
    const sockaddr_in to = loopback(r.daemon.port());
    Socket client(SOCK_DGRAM);

    // Protocol 2 discovery is answered too, by the Protocol 2 server.
    const auto p2 = p2_discovery_packet();
    ::sendto(client.fd, p2.data(), p2.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    const auto p2_reply = receive(client.fd);
    REQUIRE(p2_reply.size() == 60);
    CHECK(p2_reply[0] == 0);
    CHECK(p2_reply[4] == 0x02);
    CHECK(p2_reply[13] == 75);

    const auto discovery = discovery_packet();
    ::sendto(client.fd, discovery.data(), discovery.size(), 0,
             reinterpret_cast<const sockaddr*>(&to), sizeof to);
    const auto reply = receive(client.fd);
    REQUIRE(reply.size() == 60);
    CHECK(reply[0] == 0xef);  // the Protocol 1 reply
    CHECK(reply[8] == 9);
    CHECK(reply[9] == 75);
    CHECK(reply[21] == 3);

    // Two receivers, then start.
    const auto config = host_packet(0, {0x00, 1u << 3 | 1u << 2}, {0x04, 14074000});
    ::sendto(client.fd, config.data(), config.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    const auto start = start_packet(1);
    ::sendto(client.fd, start.data(), start.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    for (int i = 0; i < 200 && !r.daemon.server().running(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(r.daemon.server().running());
    for (int i = 0; i < 200 && r.daemon.device().snapshot().receivers != 2; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(r.daemon.device().snapshot().receivers == 2);

    feed(device, 72 * 3);
    for (int i = 0; i < 3; ++i) {
        const auto p = receive(client.fd);
        REQUIRE(p.size() == 1032);
        CHECK(p[3] == 0x06);
        CHECK(p[7] == i);
    }

    const auto stop = start_packet(0);
    ::sendto(client.fd, stop.data(), stop.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    for (int i = 0; i < 200 && r.daemon.server().running(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK_FALSE(r.daemon.server().running());
    CHECK(r.finish());

    const std::lock_guard lock(device.mutex);
    REQUIRE_FALSE(device.commands.empty());
    CHECK(device.commands.back().address == 0x09);
    CHECK_FALSE(device.commands.back().can_transmit());
    bool tuned = false;
    for (const Command& c : device.commands) {
        tuned = tuned || (c.address == 0x02 && c.data == 14074000);
    }
    CHECK(tuned);
}

TEST_CASE("radioberryd serves discovery and a stream over TCP", "[daemon]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    DaemonOptions standard = Running::options();
    standard.iq_convention = p1::IqConvention::Standard;  // logged at start
    Running r(device, standard);
    const sockaddr_in to = loopback(r.daemon.port());
    {
        Socket tcp(SOCK_STREAM);
        REQUIRE(::connect(tcp.fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0);
        const auto discovery = discovery_packet(1032);
        ::send(tcp.fd, discovery.data(), discovery.size(), 0);
        const auto reply = receive(tcp.fd);
        REQUIRE(reply.size() == 60);
        CHECK(receive(tcp.fd).empty());  // then closed
    }
    Socket tcp(SOCK_STREAM);
    REQUIRE(::connect(tcp.fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0);
    // A second connection is refused while one is open.
    Socket other(SOCK_STREAM);
    REQUIRE(::connect(other.fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0);
    CHECK(receive(other.fd).empty());

    const auto start = start_packet(1, 1032);
    // Sent in two pieces: the daemon reassembles records.
    ::send(tcp.fd, start.data(), 500, 0);
    std::this_thread::sleep_for(20ms);
    ::send(tcp.fd, start.data() + 500, start.size() - 500, 0);
    for (int i = 0; i < 200 && !r.daemon.server().running(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(r.daemon.server().running());
    CHECK(r.daemon.server().client()->tcp);
    feed(device, 126 * 2);
    for (int i = 0; i < 2; ++i) {
        const auto p = receive_all(tcp.fd, 1032);
        REQUIRE(p.size() == 1032);
        CHECK(p[7] == i);
    }
    const auto stop = start_packet(0, 1032);
    ::send(tcp.fd, stop.data(), stop.size(), 0);
    // The daemon closes the connection; the stream ends.
    std::vector<std::uint8_t> rest;
    do {
        rest = receive(tcp.fd);
    } while (!rest.empty());
    CHECK_FALSE(r.daemon.server().running());
    CHECK(r.finish());
}

TEST_CASE("radioberryd stops when the device fails", "[daemon]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    Running r(device);
    {
        const std::lock_guard lock(device.mutex);
        device.fail_on = "stats";
    }
    r.thread.join();
    CHECK_FALSE(r.result);
}

TEST_CASE("radioberryd does not start on a failed device", "[daemon]") {
    FakeDevice device;
    DaemonOptions o = Running::options();
    Daemon daemon(device, o);
    device.fail_on = "info";
    std::atomic<bool> stop{false};
    CHECK_FALSE(daemon.run(stop));
}

TEST_CASE("radioberryd reports a port already in use", "[daemon]") {
    FakeDevice device;
    Running r(device);
    DaemonOptions o = Running::options();
    o.port = r.daemon.port();
    FakeDevice second;
    CHECK_THROWS_AS(Daemon(second, o), std::system_error);
}

TEST_CASE("an interface MAC is found or not, without failing", "[daemon]") {
    const auto mac = interface_mac();
    if (mac) {
        CHECK(*mac != std::array<std::uint8_t, 6>{});
    }
    FakeDevice device;
    DaemonOptions o = Running::options();
    o.mac.reset();
    const Daemon daemon(device, o);
    CHECK(daemon.identity().mac == mac.value_or(std::array<std::uint8_t, 6>{}));
}

TEST_CASE("radioberryd transmits for the owner when allowed", "[daemon]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    DaemonOptions o = Running::options();
    o.tx.allow_tx = true;
    o.tx.drive_ceiling = 0x20;
    o.frequency_ppm = 1.0;  // logged at start
    o.iq_convention = p1::IqConvention::Hl2;
    Running r(device, o);
    const sockaddr_in to = loopback(r.daemon.port());
    Socket client(SOCK_DGRAM);
    const auto start = start_packet(1);
    ::sendto(client.fd, start.data(), start.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    // Drive 255 asked for, MOX set, every TX sample I = 0x1234, Q = 0x5678.
    for (std::uint32_t seq = 0; seq < 5; ++seq) {
        const auto p = host_packet(seq, {0x13, 0xffu << 24}, {0x01, 0}, 0x1234, 0x5678);
        ::sendto(client.fd, p.data(), p.size(), 0, reinterpret_cast<const sockaddr*>(&to),
                 sizeof to);
    }
    bool keyed = false;
    for (int i = 0; i < 400 && !keyed; ++i) {
        std::this_thread::sleep_for(5ms);
        const std::lock_guard lock(device.mutex);
        for (const Command& c : device.commands) {
            keyed = keyed || (c.ptt && (c.flags & Command::kTxPermit) != 0);
        }
        keyed = keyed && device.written.size() >= 4 * 126;
    }
    REQUIRE(keyed);
    {
        const std::lock_guard lock(device.mutex);
        CHECK(device.written[0] == 0x12);
        CHECK(device.written[3] == 0x78);
        bool capped = false;
        for (const Command& c : device.commands) {
            if (c.address == 0x09) {
                CHECK((c.data >> 24) <= 0x20);
                capped = capped || (c.data >> 24) == 0x20;
            }
        }
        CHECK(capped);
    }
    // MOX released: the next commands carry no PTT.
    const auto off = host_packet(5, {0x13, 0xffu << 24}, {0x00, 0});
    ::sendto(client.fd, off.data(), off.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    const auto stop = start_packet(0);
    ::sendto(client.fd, stop.data(), stop.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    for (int i = 0; i < 400 && r.daemon.server().running(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(r.finish());
    const std::lock_guard lock(device.mutex);
    CHECK_FALSE(device.commands.back().can_transmit());
}

TEST_CASE("driver TX underruns do not drop MOX", "[daemon]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    DaemonOptions o = Running::options();
    o.tx.allow_tx = true;
    Running r(device, o);
    const sockaddr_in to = loopback(r.daemon.port());
    Socket client(SOCK_DGRAM);
    const auto start = start_packet(1);
    ::sendto(client.fd, start.data(), start.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    const auto mox = host_packet(0, {0x02, 0}, {0x01, 0});
    ::sendto(client.fd, mox.data(), mox.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    for (int i = 0; i < 400 && !r.daemon.policy().transmitting(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(r.daemon.policy().transmitting());
    std::size_t polls = 0;
    {
        const std::lock_guard lock(device.mutex);
        device.stats_.tx_underruns += 23;  // reported at the next statistics poll
        polls = device.stats_calls;
    }
    // Two polls after the change: at least one complete poll has seen it.
    for (int i = 0; i < 400; ++i) {
        {
            const std::lock_guard lock(device.mutex);
            if (device.stats_calls >= polls + 2) {
                break;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    {
        const std::lock_guard lock(device.mutex);
        REQUIRE(device.stats_calls >= polls + 2);
    }
    CHECK_FALSE(r.daemon.session().latched());
    CHECK(r.daemon.policy().transmitting());
    CHECK(r.finish());
}

TEST_CASE("a device error while transmitting drops MOX in the daemon", "[daemon]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    DaemonOptions o = Running::options();
    o.tx.allow_tx = true;
    Running r(device, o);
    const sockaddr_in to = loopback(r.daemon.port());
    Socket client(SOCK_DGRAM);
    const auto start = start_packet(1);
    ::sendto(client.fd, start.data(), start.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    const auto mox = host_packet(0, {0x02, 0}, {0x01, 0});
    ::sendto(client.fd, mox.data(), mox.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    for (int i = 0; i < 400 && !r.daemon.policy().transmitting(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(r.daemon.policy().transmitting());
    {
        const std::lock_guard lock(device.mutex);
        device.fail_on = "write";  // the next TX block fails
    }
    const auto more = host_packet(1, {0x02, 0}, {0x01, 0});
    ::sendto(client.fd, more.data(), more.size(), 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof to);
    for (int i = 0; i < 400 && !r.daemon.session().latched(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(r.daemon.session().latched());
    CHECK(r.daemon.device().control().transmit() == TransmitState{});
    CHECK_FALSE(r.finish());  // the daemon stops on a device error
}

namespace {

// A Protocol 2 client socket: sends to the daemon's port at `offset`;
// receives with the offset of the port each packet came from.
struct P2Client {
    explicit P2Client(Daemon& d) : daemon(d) {}
    void send(unsigned offset, const std::vector<std::uint8_t>& p) {
        const sockaddr_in to = loopback(daemon.p2_port(offset));
        ::sendto(socket.fd, p.data(), p.size(), 0, reinterpret_cast<const sockaddr*>(&to),
                 sizeof to);
    }
    // The first packet from each port at `offsets`, reading until all have
    // come or a receive times out; a port with none maps to nothing.
    std::map<unsigned, std::vector<std::uint8_t>> receive_from(std::set<unsigned> offsets) {
        std::map<unsigned, std::vector<std::uint8_t>> got;
        while (got.size() < offsets.size()) {
            std::vector<std::uint8_t> b(2048);
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const ssize_t n = ::recvfrom(socket.fd, b.data(), b.size(), 0,
                                         reinterpret_cast<sockaddr*>(&from), &len);
            if (n <= 0) {
                break;
            }
            for (const unsigned offset : offsets) {
                if (ntohs(from.sin_port) == daemon.p2_port(offset) && !got.contains(offset)) {
                    b.resize(static_cast<std::size_t>(n));
                    got[offset] = b;
                }
            }
        }
        return got;
    }
    Daemon& daemon;
    Socket socket{SOCK_DGRAM};
};

}  // namespace

TEST_CASE("radioberryd serves a Protocol 2 client", "[daemon]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    DaemonOptions o = Running::options();
    o.tx.allow_tx = true;
    o.tx.drive_ceiling = 0x40;
    Running r(device, o);
    P2Client client(r.daemon);

    client.send(p2::kGeneralPort, p2_discovery_packet());
    const auto reply = client.receive_from({p2::kGeneralPort})[p2::kGeneralPort];
    REQUIRE(reply.size() == 60);
    CHECK(reply[4] == 0x02);
    CHECK(reply[11] == p2::kBoardHermesLite);
    CHECK(reply[20] == 6);

    // As piHPSDR starts: General, then (once it has taken effect, as
    // piHPSDR waits 100 ms) DDC0 and DDC1 at 96 ksps, TX, run. General
    // arrives on the base port, read by the Protocol 1 thread, the rest on
    // the Protocol 2 thread's ports.
    client.send(p2::kGeneralPort, general_packet(0));
    for (int i = 0; i < 400 && !r.daemon.session().owner(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(r.daemon.session().owner());
    client.send(p2::kRxSpecificPort, rx_specific_packet(0, {{0, 96}, {1, 96}}));
    client.send(p2::kTxSpecificPort, tx_specific_packet(0));
    HighPriorityBytes h;
    h.ddc = {phase_word(7074000), phase_word(14074000)};
    h.duc = phase_word(7074000);
    h.drive = 0xff;
    client.send(p2::kHighPriorityPort, high_priority_packet(0, h));
    for (int i = 0; i < 400 && r.daemon.device().snapshot().receivers != 2; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(r.daemon.p2_server().running());
    REQUIRE(r.daemon.device().snapshot().receivers == 2);

    feed(device, 238 * 2);
    auto streams = client.receive_from(
        {p2::kDdcPort0, p2::kDdcPort0 + 1, p2::kMicLinePort, p2::kHighPriorityStatusPort});
    REQUIRE(streams[p2::kDdcPort0 + 1].size() == p2::kLongPacketBytes);
    CHECK(streams[p2::kDdcPort0 + 1][15] == 238);
    CHECK(streams[p2::kDdcPort0].size() == p2::kLongPacketBytes);
    CHECK(streams[p2::kMicLinePort].size() == p2::kMicPacketBytes);
    CHECK(streams[p2::kHighPriorityStatusPort].size() == p2::kShortPacketBytes);

    // MOX with TX I/Q: keyed, drive capped, samples at 48 ksps.
    h.mox = true;
    client.send(p2::kHighPriorityPort, high_priority_packet(1, h));
    for (std::uint32_t seq = 0; seq < 8; ++seq) {
        client.send(p2::kTxIqPort, tx_iq_packet(seq, 0x123400, 0));
    }
    bool keyed = false;
    for (int i = 0; i < 400 && !keyed; ++i) {
        std::this_thread::sleep_for(5ms);
        const std::lock_guard lock(device.mutex);
        for (const Command& c : device.commands) {
            keyed = keyed || (c.ptt && (c.flags & Command::kTxPermit) != 0);
        }
        keyed = keyed && device.written.size() >= 8 * 60 * kTxSampleBytes;
    }
    REQUIRE(keyed);
    {
        const std::lock_guard lock(device.mutex);
        bool capped = false;
        bool tuned = false;
        for (const Command& c : device.commands) {
            if (c.address == 0x09) {
                CHECK((c.data >> 24) <= 0x40);
                capped = capped || (c.data >> 24) == 0x40;
            }
            tuned = tuned || (c.address == 0x03 && c.data == 14074000);
        }
        CHECK(capped);
        CHECK(tuned);
        const std::uint8_t* last = device.written.data() + device.written.size() - kTxSampleBytes;
        CHECK(std::abs(static_cast<std::int16_t>(last[0] << 8 | last[1]) - 0x1234) <= 1);
    }

    // Run clear: released, safe.
    h.run = false;
    h.mox = false;
    client.send(p2::kHighPriorityPort, high_priority_packet(2, h));
    for (int i = 0; i < 400 && r.daemon.session().owner(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK_FALSE(r.daemon.session().owner());
    CHECK_FALSE(r.daemon.p2_server().running());
    CHECK(r.finish());
    const std::lock_guard lock(device.mutex);
    CHECK_FALSE(device.commands.back().can_transmit());
}

#if !defined(__SANITIZE_ADDRESS__)
// Not under ASan: locking memory would try to lock its shadow mappings.
TEST_CASE("radioberryd runs with real-time scheduling where allowed", "[daemon]") {
    FakeDevice device;
    device.idle_wait_ms = 1;
    DaemonOptions o = Running::options();
    o.realtime = true;
    Running r(device, o);
    std::this_thread::sleep_for(50ms);  // the threads start and set themselves
    CHECK(r.finish());
}
#endif
