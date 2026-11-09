// SPDX-License-Identifier: GPL-2.0-or-later
#include "realtime.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstring>

namespace radioberry::daemon {

void name_thread(const char* name) { ::pthread_setname_np(::pthread_self(), name); }

std::optional<std::string> make_thread_realtime(const char* name, int priority) {
    sched_param param{};
    param.sched_priority = priority;
    const int e = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
    if (e != 0) {
        return std::string("SCHED_FIFO ") + std::to_string(priority) + " for " + name + ": " +
               std::strerror(e);
    }
    return std::nullopt;
}

std::optional<std::string> lock_memory() {
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        return std::string("locking memory: ") + std::strerror(errno);
    }
    return std::nullopt;
}

}  // namespace radioberry::daemon
