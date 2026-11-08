// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "device/monitor.hpp"
#include "device/ring.hpp"
#include "radioberry/device.hpp"

namespace radioberry::daemon {

// TX samples for the driver: I then Q, 16-bit big-endian, at 48 ksps.
struct TxBlock {
    static constexpr std::size_t kCapacity = 256 * kTxSampleBytes;
    std::array<std::uint8_t, kCapacity> bytes{};
    std::size_t size = 0;  // a whole number of samples
};

// The TX device thread's work: blocks from the ring into the driver, in
// order, each written whole before the next. Nothing is written when the
// ring is empty; the driver and the gateware's PTT hang-time watchdog then
// end any transmission.
class TxPump {
public:
    // With `ring_limit_words` above 0, a block that would take the
    // driver's TX ring above that many samples is dropped rather than
    // queued, which bounds the transmit latency.
    TxPump(Device& device, SpscRing<TxBlock>& ring, FaultSink sink, int wait_ms,
           std::uint32_t ring_limit_words = 0);

    // One pass: waits for a block (until `stop` or a ring wake-up), then
    // up to wait_ms for room in the driver, and writes what fits. A block
    // that is not whole samples is dropped with a tx-invalid-block fault.
    // Returns false after a device error, which is reported as a fault and
    // ends the pump.
    bool step(const std::atomic<bool>& stop);

    bool failed() const { return failed_.load(); }
    std::uint64_t blocks() const { return blocks_.load(); }
    std::uint64_t bytes() const { return bytes_.load(); }
    std::uint64_t blocks_dropped() const { return blocks_dropped_.load(); }

private:
    Device& device_;
    SpscRing<TxBlock>& ring_;
    FaultSink sink_;
    int wait_ms_;
    std::uint32_t ring_limit_words_;
    std::size_t offset_ = 0;  // bytes of the front block already written

    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> blocks_dropped_{0};
};

}  // namespace radioberry::daemon
