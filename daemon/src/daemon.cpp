// SPDX-License-Identifier: GPL-2.0-or-later
#include "daemon.hpp"

#include <spdlog/spdlog.h>

#include <thread>

namespace radioberry::daemon {
namespace {

p1::Identity identify(Device& device, const std::optional<std::array<std::uint8_t, 6>>& mac) {
    const radioberry_info info = device.info();
    p1::Identity id;
    id.mac = mac ? *mac : interface_mac().value_or(std::array<std::uint8_t, 6>{});
    id.version_major = info.gateware_major;
    id.version_minor = info.gateware_minor;
    id.receivers = info.receivers;
    return id;
}

void log_fault(const Fault& fault) {
    if (fault.kind == FaultKind::DeviceError) {
        spdlog::error("{}", describe(fault));
    } else {
        spdlog::warn("{}", describe(fault));
    }
}

}  // namespace

Daemon::Daemon(Device& device, DaemonOptions options)
    : identity_(identify(device, options.mac)),
      service_(device, log_fault, options.device),
      network_(options.address, options.port),
      server_(
          identity_, service_.control(), service_.tx(),
          [this] {
              const DeviceSnapshot s = service_.snapshot();
              return p1::GatewareStatus{s.cw_or_ptt, s.external_cw_key};
          },
          network_, options.server) {}

Daemon::~Daemon() = default;

bool Daemon::run(const std::atomic<bool>& stop) {
    spdlog::info(
        "gateware {}.{}, {} receivers; Protocol 1 on port {}; this version does not "
        "transmit and claims no PA interlock",
        identity_.version_major, identity_.version_minor, identity_.receivers, port());
    if (!service_.start()) {
        return false;
    }
    std::atomic<bool> stop_rx{false};
    std::thread rx([&] {
        while (!stop_rx) {
            service_.rx().wait(stop_rx);
            while (const RxBlock* block = service_.rx().front()) {
                server_.on_rx(*block);
                service_.rx().pop();
            }
        }
    });
    const Clock clock = steady_clock();
    const auto handler = [&](std::span<const std::uint8_t> packet, const p1::Peer& from) {
        server_.on_packet(packet, from, clock());
    };
    while (!stop && !service_.failed()) {
        network_.poll(100, handler);
        server_.tick(clock());
    }
    const bool failed = service_.failed();
    stop_rx = true;
    service_.rx().wake();
    rx.join();
    service_.stop();
    if (failed) {
        spdlog::error("stopped: the device failed");
    }
    return !failed;
}

}  // namespace radioberry::daemon
