// SPDX-License-Identifier: GPL-2.0-or-later
//
// The UDP transport on loopback, and the clients' handling of stray
// datagrams, silence and being destroyed mid-stream.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "client.hpp"
#include "fake_radio.hpp"

using namespace radioberry::proto;
using radioberry::proto::test::FakeRadio;
using namespace std::chrono_literals;

namespace {

// Running out of descriptors upsets the sanitizer runtimes (their own type
// checks fail), so the sanitizer build skips those cases.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

// A UDP socket on `address`, port chosen by the kernel.
struct Udp {
    explicit Udp(const char* address) : fd(::socket(AF_INET, SOCK_DGRAM, 0)) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        ::inet_pton(AF_INET, address, &a.sin_addr);
        REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0);
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        const timeval t{2, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    }
    ~Udp() { ::close(fd); }
    Udp(const Udp&) = delete;
    Udp& operator=(const Udp&) = delete;

    // Receives one datagram and returns the sender's port.
    std::uint16_t receive_from() {
        std::uint8_t b[2048];
        sockaddr_in from{};
        socklen_t len = sizeof from;
        REQUIRE(::recvfrom(fd, b, sizeof b, 0, reinterpret_cast<sockaddr*>(&from), &len) > 0);
        return ntohs(from.sin_port);
    }
    void send_to(std::uint16_t to_port, std::size_t bytes) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(0x7f000001);
        to.sin_port = htons(to_port);
        const std::vector<std::uint8_t> b(bytes, 0x5a);
        REQUIRE(::sendto(fd, b.data(), b.size(), 0, reinterpret_cast<const sockaddr*>(&to),
                         sizeof to) == static_cast<ssize_t>(bytes));
    }

    int fd;
    std::uint16_t port = 0;
};

}  // namespace

TEST_CASE("the UDP transport sends, receives and labels by port offset", "[protoprobe]") {
    Udp radio("127.0.0.1");
    const auto transport = open_udp("127.0.0.1", radio.port);
    const std::vector<std::uint8_t> hello(10, 1);
    transport->send(0, hello);
    const std::uint16_t client_port = radio.receive_from();

    // From the radio's base port: offset 0.
    radio.send_to(client_port, 60);
    auto d = transport->receive(1000ms);
    REQUIRE(d);
    CHECK(d->offset == 0);
    CHECK(d->bytes.size() == 60);

    // From another address: not the radio.
    Udp stranger("127.0.0.2");
    stranger.send_to(client_port, 8);
    d = transport->receive(1000ms);
    REQUIRE(d);
    CHECK(d->offset == ~0u);

    // Nothing more: a timeout.
    CHECK_FALSE(transport->receive(10ms));

    // A datagram the kernel refuses to send.
    CHECK_THROWS_AS(transport->send(0, std::vector<std::uint8_t>(70000, 0)), std::system_error);
}

TEST_CASE("the UDP transport reports what it cannot set up", "[protoprobe]") {
    CHECK_THROWS_AS(open_udp("radio.local", 1024), std::runtime_error);
    if (kSanitized) {
        return;
    }
    rlimit old{};
    ::getrlimit(RLIMIT_NOFILE, &old);
    const int lowest_free = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    ::close(lowest_free);
    rlimit none = old;
    none.rlim_cur = static_cast<rlim_t>(lowest_free);
    ::setrlimit(RLIMIT_NOFILE, &none);
    CHECK_THROWS_AS(open_udp("127.0.0.1", 1024), std::system_error);
    ::setrlimit(RLIMIT_NOFILE, &old);
}

TEST_CASE("the clients skip stray datagrams and report silence", "[protoprobe]") {
    for (const bool two : {false, true}) {
        INFO((two ? "Protocol 2" : "Protocol 1"));
        FakeRadio radio(two);
        const auto client = two ? make_p2_client(radio, radio.clock()) : make_p1_client(radio);
        // A stray datagram arrives before the discovery reply.
        radio.queued.push_back({0, std::vector<std::uint8_t>(7, 0)});
        const auto found = client->discover(100ms);
        REQUIRE(found);
        CHECK(found->version_major == 75);

        // Nothing streaming: poll times out.
        CHECK_FALSE(client->poll(10ms));

        radio.answer = false;
        CHECK_FALSE(client->discover(10ms));

        StreamConfig c;
        client->start(c);
        // A stray datagram in the stream is skipped; the next packet counts.
        radio.queued.push_back({4, std::vector<std::uint8_t>(9, 0)});
        CHECK(client->poll(10ms));
        CHECK(client->counts().packets == 1);
    }
}

TEST_CASE("a client destroyed mid-stream stops the radio", "[protoprobe]") {
    for (const bool two : {false, true}) {
        FakeRadio radio(two);
        {
            const auto client = two ? make_p2_client(radio, radio.clock()) : make_p1_client(radio);
            REQUIRE(client->discover(100ms));
            client->start(StreamConfig{});
            CHECK(radio.streaming);
        }
        CHECK_FALSE(radio.streaming);
    }
}

TEST_CASE("p1probe streams at every Protocol 1 rate", "[protoprobe]") {
    for (const unsigned rate : {48u, 96u, 192u, 384u}) {
        INFO("rate " << rate);
        FakeRadio radio(false);
        const auto client = make_p1_client(radio);
        StreamConfig c;
        c.rate_ksps = rate;
        client->start(c);
        CHECK(radio.rate_ksps == rate);
        CHECK(client->expected_packets_per_second() == rate * 1000.0 / 126);
        client->stop();
    }
}
