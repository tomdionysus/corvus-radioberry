// SPDX-License-Identifier: GPL-2.0-or-later
#include "device/service.hpp"

#include <utility>

namespace radioberry::daemon {

DeviceService::DeviceService(Device& device, FaultSink sink, DeviceOptions options, Clock clock)
    : rx_ring_(options.rx_blocks),
      tx_ring_(options.tx_blocks),
      monitor_(sink, options.stall_after),
      rx_pump_(device, control_, rx_ring_, monitor_, sink, std::move(clock),
               {options.stats_period, options.wait_ms}),
      tx_pump_(device, tx_ring_, sink, options.wait_ms, options.tx_ring_limit_words),
      on_thread_start_(std::move(options.on_thread_start)) {}

DeviceService::~DeviceService() { stop(); }

bool DeviceService::start() {
    if (running_) {
        return true;
    }
    if (!rx_pump_.start()) {
        return false;
    }
    stop_rx_ = false;
    stop_tx_ = false;
    rx_thread_ = std::thread([this] {
        if (on_thread_start_) {
            on_thread_start_("rx");
        }
        while (!stop_rx_ && rx_pump_.step()) {
        }
        rx_pump_.flush_commands();
    });
    tx_thread_ = std::thread([this] {
        if (on_thread_start_) {
            on_thread_start_("tx");
        }
        while (!stop_tx_ && tx_pump_.step(stop_tx_)) {
        }
    });
    running_ = true;
    return true;
}

void DeviceService::stop() {
    if (!running_) {
        return;
    }
    control_.safe();
    stop_rx_ = true;
    rx_thread_.join();
    stop_tx_ = true;
    tx_ring_.wake();
    tx_thread_.join();
    rx_ring_.wake();
    running_ = false;
}

DeviceSnapshot DeviceService::snapshot() const {
    DeviceSnapshot s;
    s.driver = monitor_.last();
    s.receivers = rx_pump_.receivers();
    s.rate_code = rx_pump_.rate_code();
    s.commands = rx_pump_.commands();
    s.rx_blocks = rx_pump_.blocks();
    s.rx_blocks_dropped = rx_pump_.blocks_dropped();
    s.tx_blocks = tx_pump_.blocks();
    s.tx_bytes = tx_pump_.bytes();
    s.tx_blocks_dropped = tx_pump_.blocks_dropped();
    s.cw_or_ptt = rx_pump_.cw_or_ptt();
    s.external_cw_key = rx_pump_.external_cw_key();
    s.stalled = monitor_.stalled();
    s.failed = failed();
    return s;
}

}  // namespace radioberry::daemon
