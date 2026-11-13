// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Protocol 1 and Protocol 2 sockets on loopback, including the ways
// they fail: ports already taken, no file descriptors left, sends the
// kernel refuses and a TCP peer that resets.
#include "net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "p2/network.hpp"

using namespace radioberry::daemon;

namespace {

constexpr std::uint32_t kLoopback = 0x7f000001;

// Running out of descriptors upsets the sanitizer runtimes (their own type
// checks fail), so the sanitizer build skips those cases.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(kLoopback);
    a.sin_port = htons(port);
    return a;
}

// A socket bound to a free loopback port, holding it.
struct Held {
    explicit Held(int type) : fd(::socket(AF_INET, type, 0)) {
        sockaddr_in a = loopback(0);
        REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0);
        if (type == SOCK_STREAM) {
            REQUIRE(::listen(fd, 1) == 0);
        }
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
    }
    ~Held() { ::close(fd); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    int fd;
    std::uint16_t port = 0;
};

// Lowers the open-file limit so that only `spare` more descriptors can be
// opened, until destroyed.
struct FdLimit {
    explicit FdLimit(int spare) {
        ::getrlimit(RLIMIT_NOFILE, &old);
        const int lowest_free = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        ::close(lowest_free);
        rlimit lowered = old;
        lowered.rlim_cur = static_cast<rlim_t>(lowest_free + spare);
        ::setrlimit(RLIMIT_NOFILE, &lowered);
    }
    ~FdLimit() { ::setrlimit(RLIMIT_NOFILE, &old); }
    rlimit old{};
};

std::string what_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const std::system_error& e) {
        return e.what();
    }
    return "no exception";
}

}  // namespace

TEST_CASE("the Protocol 1 sockets report what they cannot set up", "[net]") {
    const Held udp(SOCK_DGRAM);
    CHECK(what_of([&] { Network n(kLoopback, udp.port); }).find("UDP bind to port") !=
          std::string::npos);
    // The UDP port free but its TCP twin taken.
    const Held tcp(SOCK_STREAM);
    CHECK(what_of([&] { Network n(kLoopback, tcp.port); }).find("TCP bind to port") !=
          std::string::npos);
    if (kSanitized) {
        return;
    }
    {
        const FdLimit none(0);
        CHECK(what_of([] { Network n(kLoopback, 0); }).find("UDP socket") != std::string::npos);
    }
    {
        const FdLimit one(1);
        CHECK(what_of([] { Network n(kLoopback, 0); }).find("TCP socket") != std::string::npos);
    }
}

TEST_CASE("the Protocol 1 sockets count the sends that fail", "[net]") {
    Network n(kLoopback, 0);
    const Held client(SOCK_DGRAM);
    const std::vector<std::uint8_t> small(16, 0);
    const std::vector<std::uint8_t> oversized(70000, 0);  // above a datagram's limit
    n.send(Peer{kLoopback, client.port, false}, small);
    CHECK(n.send_failures() == 0);
    n.send(Peer{kLoopback, client.port, false}, oversized);
    CHECK(n.send_failures() == 1);
    // TCP with no connection.
    n.send(Peer{kLoopback, client.port, true}, small);
    CHECK(n.send_failures() == 2);
}

namespace {

// Every datagram waiting on `fd`, in order.
std::vector<std::vector<std::uint8_t>> drain(int fd) {
    std::vector<std::vector<std::uint8_t>> got;
    std::vector<std::uint8_t> b(2048);
    for (;;) {
        const ssize_t n = ::recv(fd, b.data(), b.size(), MSG_DONTWAIT);
        if (n < 0) {
            return got;
        }
        got.emplace_back(b.begin(), b.begin() + n);
    }
}

}  // namespace

TEST_CASE("a batch goes out in order, with the failures counted", "[net]") {
    Network n(kLoopback, 0);
    const Held client(SOCK_DGRAM);
    const int room = 1 << 20;
    ::setsockopt(client.fd, SOL_SOCKET, SO_RCVBUF, &room, sizeof room);
    const Peer to{kLoopback, client.port, false};
    // More than one sendmmsg() chunk, in different sizes; one too large.
    std::vector<std::vector<std::uint8_t>> packets;
    for (std::uint8_t k = 0; k < 150; ++k) {
        packets.emplace_back(k == 70 ? 70000 : 100u + k, k);
    }
    std::vector<std::span<const std::uint8_t>> spans(packets.begin(), packets.end());
    n.send_batch(to, spans);
    CHECK(n.send_failures() == 1);
    const auto got = drain(client.fd);
    REQUIRE(got.size() == 149);
    for (std::size_t i = 0; i < got.size(); ++i) {
        const std::uint8_t k = static_cast<std::uint8_t>(i < 70 ? i : i + 1);
        CHECK(got[i].size() == 100u + k);
        CHECK(got[i][0] == k);
    }
    // Over TCP with no connection, each packet fails.
    n.send_batch(Peer{kLoopback, client.port, true}, std::span(spans).first(3));
    CHECK(n.send_failures() == 4);
    CHECK(send_datagrams(-1, kLoopback, client.port, std::span(spans).first(5)) == 5);
}

TEST_CASE("a Protocol 2 batch goes out from each packet's port", "[net]") {
    std::size_t base_sends = 0;
    p2::Network n(kLoopback, 0, 2,
                  [&](const Peer&, std::span<const std::uint8_t>) { ++base_sends; });
    const Held client(SOCK_DGRAM);
    const Peer to{kLoopback, client.port, false};
    const std::vector<std::uint8_t> a(60, 1);
    const std::vector<std::uint8_t> b(60, 2);
    const std::vector<p2::Link::Datagram> batch = {
        {p2::kMicLinePort, a},  {p2::kDdcPort0, a},    {p2::kDdcPort0, b},
        {p2::kDdcPort0 + 1, b}, {p2::kGeneralPort, a}, {p2::kDdcPort0 + 5, a},
        {p2::kDdcPort0 + 1, a},
    };
    n.send_batch(to, batch);
    CHECK(base_sends == 1);
    CHECK(n.send_failures() == 1);  // no such DDC
    std::vector<std::uint16_t> from;
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> buf(2048);
    for (;;) {
        sockaddr_in src{};
        socklen_t len = sizeof src;
        const ssize_t got = ::recvfrom(client.fd, buf.data(), buf.size(), MSG_DONTWAIT,
                                       reinterpret_cast<sockaddr*>(&src), &len);
        if (got < 0) {
            break;
        }
        from.push_back(ntohs(src.sin_port));
        first.push_back(buf[0]);
    }
    const std::vector<std::uint16_t> ports = {
        n.port(p2::kMicLinePort),  n.port(p2::kDdcPort0),     n.port(p2::kDdcPort0),
        n.port(p2::kDdcPort0 + 1), n.port(p2::kDdcPort0 + 1),
    };
    CHECK(from == ports);
    CHECK(first == std::vector<std::uint8_t>{1, 1, 2, 2, 1});
}

TEST_CASE("the Protocol 1 TCP connection survives a peer reset", "[net]") {
    Network n(kLoopback, 0);
    const auto ignore = [](std::span<const std::uint8_t>, const Peer&) {};

    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    const sockaddr_in to = loopback(n.port());
    REQUIRE(::connect(client, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0);
    sockaddr_in local{};
    socklen_t len = sizeof local;
    ::getsockname(client, reinterpret_cast<sockaddr*>(&local), &len);
    const Peer peer{kLoopback, ntohs(local.sin_port), true};
    n.poll(500, ignore);  // accepts

    // The client resets the connection; sends fail, and the poll thread
    // closes the socket on the reset.
    const linger reset{1, 0};
    ::setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof reset);
    ::close(client);
    const std::vector<std::uint8_t> record(1032, 0);
    for (int i = 0; i < 3; ++i) {
        n.send(peer, record);
    }
    CHECK(n.send_failures() >= 1);
    n.poll(200, ignore);
    // The connection is gone: a send to it now fails without trying.
    const std::uint64_t before = n.send_failures();
    n.send(peer, record);
    CHECK(n.send_failures() == before + 1);

    // A new connection is accepted after the old one closed.
    const int second = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(::connect(second, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0);
    n.poll(500, ignore);
    len = sizeof local;
    ::getsockname(second, reinterpret_cast<sockaddr*>(&local), &len);
    const std::uint64_t now = n.send_failures();
    n.send(Peer{kLoopback, ntohs(local.sin_port), true}, record);
    CHECK(n.send_failures() == now);
    ::close(second);
}

TEST_CASE("the Protocol 2 sockets report what they cannot set up", "[net]") {
    const auto base = [](const Peer&, std::span<const std::uint8_t>) {};
    // Receiver Specific (base + 1) already taken.
    const Held taken(SOCK_DGRAM);
    const auto b = static_cast<std::uint16_t>(taken.port - 1);
    CHECK(what_of([&] { p2::Network n(kLoopback, b, 2, base); }).find("UDP bind to port") !=
          std::string::npos);
    if (kSanitized) {
        return;
    }
    const FdLimit one(1);
    CHECK(what_of([&] { p2::Network n(kLoopback, 0, 2, base); }).find("UDP socket") !=
          std::string::npos);
}

TEST_CASE("the Protocol 2 sockets count the sends that fail", "[net]") {
    std::size_t base_sends = 0;
    p2::Network n(kLoopback, 0, 1,
                  [&](const Peer&, std::span<const std::uint8_t>) { ++base_sends; });
    const Held client(SOCK_DGRAM);
    const Peer to{kLoopback, client.port, false};
    const std::vector<std::uint8_t> small(60, 0);
    n.send(p2::kGeneralPort, to, small);
    CHECK(base_sends == 1);
    n.send(p2::kHighPriorityStatusPort, to, small);
    CHECK(n.send_failures() == 0);
    n.send(p2::kDdcPort0 + 5, to, small);  // no such DDC
    CHECK(n.send_failures() == 1);
    n.send(p2::kMicLinePort, to, std::vector<std::uint8_t>(70000, 0));
    CHECK(n.send_failures() == 2);
}
