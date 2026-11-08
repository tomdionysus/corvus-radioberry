// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/tx_pump.hpp"

#include <exception>
#include <string>
#include <utility>

namespace radioberry::daemon {

TxPump::TxPump(Device& device, SpscRing<TxBlock>& ring, FaultSink sink, int wait_ms)
    : device_(device), ring_(ring), sink_(std::move(sink)), wait_ms_(wait_ms) {}

bool TxPump::step(const std::atomic<bool>& stop) {
    if (failed_) {
        return false;
    }
    TxBlock* block = ring_.front();
    if (block == nullptr) {
        ring_.wait(stop);
        return true;
    }
    if (block->size % kTxSampleBytes != 0 || block->size > TxBlock::kCapacity) {
        sink_({FaultKind::TxInvalidBlock, 1,
               "TX block of " + std::to_string(block->size) + " bytes dropped"});
        ring_.pop();
        offset_ = 0;
        return true;
    }
    try {
        if (offset_ < block->size) {
            if (!device_.wait(false, true, wait_ms_).writable) {
                return true;
            }
            const std::size_t n = device_.write(std::span<const std::uint8_t>(
                block->bytes.data() + offset_, block->size - offset_));
            offset_ += n;
            bytes_ += n;
        }
        if (offset_ >= block->size) {
            ring_.pop();
            offset_ = 0;
            ++blocks_;
        }
        return true;
    } catch (const std::exception& e) {
        failed_ = true;
        sink_({FaultKind::DeviceError, 1, e.what()});
        return false;
    }
}

}  // namespace radioberry::daemon
