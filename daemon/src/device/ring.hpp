// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
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
    // front() afterwards.
    void wait(const std::atomic<bool>& stop) {
        const std::uint32_t seen = signal_.load(std::memory_order_acquire);
        if (stop.load(std::memory_order_acquire) || !empty()) {
            return;
        }
        signal_.wait(seen, std::memory_order_acquire);
    }
    // Releases a consumer blocked in wait(); set `stop` first to end it.
    void wake() {
        signal_.fetch_add(1, std::memory_order_release);
        signal_.notify_all();
    }

private:
    std::size_t advance(std::size_t i) const { return i + 1 == slots_.size() ? 0 : i + 1; }

    std::vector<T> slots_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::atomic<std::uint32_t> signal_{0};
};

}  // namespace radioberry::daemon
