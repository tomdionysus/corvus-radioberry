// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string>

namespace radioberry::daemon {

// Real-time priorities for radioberryd's threads: SCHED_FIFO, below the
// driver's own kernel thread (SCHED_FIFO 50), devices above network.
inline constexpr int kDevicePriority = 20;
inline constexpr int kNetworkPriority = 10;

// Names the calling thread (at most 15 characters). The main thread is
// left alone: its name is the process's.
void name_thread(const char* name);

// Puts the calling thread, called `name` in the message, under SCHED_FIFO
// at `priority`. Nothing on success; otherwise why it failed (without
// CAP_SYS_NICE, for example).
std::optional<std::string> make_thread_realtime(const char* name, int priority);

// Locks the process's memory now and in the future (mlockall), so a
// page fault cannot stall a stream thread. Nothing on success; otherwise
// why it failed (without CAP_IPC_LOCK, for example).
std::optional<std::string> lock_memory();

}  // namespace radioberry::daemon
