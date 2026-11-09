// SPDX-License-Identifier: GPL-2.0-or-later
#include "realtime.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>

using namespace radioberry::daemon;

TEST_CASE("threads are named and made real-time where allowed", "[realtime]") {
    std::thread([] {
        name_thread("rb-test");
        char name[16] = {};
        ::pthread_getname_np(::pthread_self(), name, sizeof name);
        CHECK(std::string(name) == "rb-test");

        // As root (the CI gate on the Pi) this succeeds; otherwise it says why.
        if (const auto failed = make_thread_realtime("rb-test", 5)) {
            CHECK(failed->find("SCHED_FIFO 5 for rb-test") == 0);
        } else {
            CHECK(::sched_getscheduler(0) == SCHED_FIFO);
        }
        // Outside SCHED_FIFO's range: always refused, with the reason.
        const auto refused = make_thread_realtime("rb-test", 1000);
        REQUIRE(refused);
        CHECK(refused->find("SCHED_FIFO 1000 for rb-test: ") == 0);
    }).join();
}

#if !defined(__SANITIZE_ADDRESS__)
// Under ASan the shadow memory is too large to lock.
TEST_CASE("memory is locked where allowed", "[realtime]") {
    if (const auto failed = lock_memory()) {
        CHECK(failed->find("locking memory: ") == 0);
    } else {
        ::munlockall();
    }
}
#endif
