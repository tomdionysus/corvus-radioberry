// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace radioberry::daemon {

// Lock-free single-producer single-consumer ring of preallocated slots,
// for the streams between the device threads and the protocol threads.
// The producer fills acquire() and publishes it; the consumer reads
// front() and frees it with pop(). Neither side allocates or locks.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity) : slots_(capacity + 1) {
        if (capacity == 0) {
            throw std::invalid_argument("ring capacity 0");
        }
    }
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const { return slots_.size() - 1; }

    // Producer: the next free slot, or nullptr when the ring is full.
    T* acquire() {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (advance(head) == tail_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return &slots_[head];
    }
    // Producer: hands the slot from acquire() to the consumer.
    void publish() {
        head_.store(advance(head_.load(std::memory_order_relaxed)), std::memory_order_release);
        wake();
    }

    // Consumer: the oldest published slot, or nullptr when empty.
    T* front() {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return &slots_[tail];
    }
    // Consumer: frees the slot from front().
    void pop() {
        tail_.store(advance(tail_.load(std::memory_order_relaxed)), std::memory_order_release);
    }

    std::size_t size() const {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head >= tail ? head - tail : slots_.size() - (tail - head);
    }
    bool empty() const { return size() == 0; }

    // Consumer: blocks until a slot is published, wake() is called or
    // `stop` is set; returns at once if either already holds. Check
    // front() afterwards. It sleeps in the kernel without spinning first
    // (std::atomic::wait spins and yields, which at real-time priority
    // with a block every millisecond costs most of a core).
    void wait(const std::atomic<bool>& stop) {
        const std::uint32_t seen = signal_.load(std::memory_order_seq_cst);
        if (stop.load(std::memory_order_acquire) || !empty()) {
            return;
        }
        // Announce the sleep, then look again: a wake() after this store
        // sees sleeping_ and calls the kernel; one before it changed
        // signal_, so the futex returns at once.
        sleeping_.store(true, std::memory_order_seq_cst);
        if (!stop.load(std::memory_order_acquire) && empty()) {
            futex_wait(seen);
        }
        sleeping_.store(false, std::memory_order_relaxed);
    }
    // Releases a consumer blocked in wait(); set `stop` first to end it.
    // Enters the kernel only when the consumer is asleep.
    void wake() {
        signal_.fetch_add(1, std::memory_order_seq_cst);
        if (sleeping_.load(std::memory_order_seq_cst)) {
            futex_wake();
        }
    }

private:
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t) &&
                      std::atomic<std::uint32_t>::is_always_lock_free,
                  "the futex word must be a plain 32-bit integer");

    std::size_t advance(std::size_t i) const { return i + 1 == slots_.size() ? 0 : i + 1; }

    std::uint32_t* futex_word() { return reinterpret_cast<std::uint32_t*>(&signal_); }
    // Sleeps while signal_ still holds `seen`; a signal or spurious return
    // is fine, the caller checks again.
    void futex_wait(std::uint32_t seen) {
        syscall(SYS_futex, futex_word(), FUTEX_WAIT_PRIVATE, seen, nullptr, nullptr, 0);
    }
    void futex_wake() {
        syscall(SYS_futex, futex_word(), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
    }

    std::vector<T> slots_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::atomic<std::uint32_t> signal_{0};
    std::atomic<bool> sleeping_{false};
};

}  // namespace radioberry::daemon
