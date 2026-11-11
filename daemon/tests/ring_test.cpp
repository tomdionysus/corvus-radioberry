// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/ring.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>

using radioberry::daemon::SpscRing;

TEST_CASE("a ring refuses capacity 0", "[ring]") {
    CHECK_THROWS_AS(SpscRing<int>(0), std::invalid_argument);
}

TEST_CASE("a ring passes slots in order and refuses more than its capacity", "[ring]") {
    SpscRing<int> ring(3);
    CHECK(ring.capacity() == 3);
    CHECK(ring.empty());
    CHECK(ring.front() == nullptr);
    for (int round = 0; round < 5; ++round) {  // wraps the indices
        for (int i = 0; i < 3; ++i) {
            int* slot = ring.acquire();
            REQUIRE(slot != nullptr);
            *slot = round * 10 + i;
            ring.publish();
        }
        CHECK(ring.acquire() == nullptr);
        CHECK(ring.size() == 3);
        for (int i = 0; i < 3; ++i) {
            const int* slot = ring.front();
            REQUIRE(slot != nullptr);
            CHECK(*slot == round * 10 + i);
            ring.pop();
            CHECK(ring.size() == static_cast<std::size_t>(2 - i));
        }
        CHECK(ring.empty());
    }
}

TEST_CASE("a ring's size is right after the head wraps behind the tail", "[ring]") {
    SpscRing<int> ring(3);
    for (int i = 0; i < 3; ++i) {
        ring.acquire();
        ring.publish();
    }
    ring.pop();
    ring.pop();
    ring.acquire();
    ring.publish();  // head wrapped to 0, tail at 2
    CHECK(ring.size() == 2);
}

TEST_CASE("a ring's wait returns at once when data or stop is there", "[ring]") {
    SpscRing<int> ring(2);
    std::atomic<bool> stop{true};
    ring.wait(stop);
    stop = false;
    ring.acquire();
    ring.publish();
    ring.wait(stop);
    CHECK(ring.front() != nullptr);
}

TEST_CASE("a ring's wait wakes on publish and on wake", "[ring]") {
    SpscRing<int> ring(2);
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        *ring.acquire() = 7;
        ring.publish();
    });
    ring.wait(stop);
    producer.join();
    REQUIRE(ring.front() != nullptr);
    CHECK(*ring.front() == 7);
    ring.pop();

    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop = true;
        ring.wake();
    });
    ring.wait(stop);
    stopper.join();
    CHECK(ring.front() == nullptr);
}

TEST_CASE("a ring's wait loses no wake-up between a sleeping consumer and a producer", "[ring]") {
    // The producer publishes as the consumer goes to sleep, again and
    // again; a lost wake-up leaves the consumer asleep with a slot waiting,
    // and the watchdog below fails the test instead of hanging it.
    constexpr int kItems = 200000;
    SpscRing<int> ring(4);
    std::atomic<bool> stop{false};
    std::atomic<int> received{0};
    std::thread consumer([&] {
        int expected = 0;
        while (expected < kItems) {
            ring.wait(stop);
            if (stop) {
                return;
            }
            while (const int* slot = ring.front()) {
                if (*slot != expected) {
                    return;
                }
                ++expected;
                ring.pop();
            }
            received = expected;
        }
    });
    std::thread producer([&] {
        for (int i = 0; i < kItems && !stop; ++i) {
            int* slot = nullptr;
            while ((slot = ring.acquire()) == nullptr && !stop) {
                std::this_thread::yield();
            }
            if (slot == nullptr) {
                return;
            }
            *slot = i;
            ring.publish();
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (received < kItems && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const int got = received;
    stop = true;
    ring.wake();
    producer.join();
    consumer.join();
    CHECK(got == kItems);
}
