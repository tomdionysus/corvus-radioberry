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
#include <thread>
#include <vector>

#include "fake_device.hpp"
#include "p1_packets.hpp"

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
    explicit Running(FakeDevice& device) : daemon(device, options()) {
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

    const auto discovery = discovery_packet();
    ::sendto(client.fd, discovery.data(), discovery.size(), 0,
             reinterpret_cast<const sockaddr*>(&to), sizeof to);
    const auto reply = receive(client.fd);
    REQUIRE(reply.size() == 60);
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
    Running r(device);
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
