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

// Faults that break the transmit stream: the session drops MOX on them.
bool breaks_transmit(FaultKind kind) {
    switch (kind) {
        case FaultKind::TxUnderrun:
        case FaultKind::TxInvalidBlock:
        case FaultKind::RxQueueFull:
        case FaultKind::DeviceError:
            return true;
        default:
            return false;
    }
}

}  // namespace

Daemon::Daemon(Device& device, DaemonOptions options)
    : identity_(identify(device, options.mac)),
      session_([this] { service_.control().safe(); }, options.client_timeout),
      service_(
          device, [this](const Fault& fault) { on_fault(fault); }, options.device),
      network_(options.address, options.port),
      server_(
          identity_, service_.control(), service_.tx(), session_,
          [this] {
              const DeviceSnapshot s = service_.snapshot();
              return p1::GatewareStatus{s.cw_or_ptt, s.external_cw_key};
          },
          network_) {}

Daemon::~Daemon() = default;

void Daemon::on_fault(const Fault& fault) {
    if (fault.kind == FaultKind::DeviceError) {
        spdlog::error("{}", describe(fault));
    } else {
        spdlog::warn("{}", describe(fault));
    }
    if (breaks_transmit(fault.kind)) {
        session_.fault(describe(fault));
    }
}

void Daemon::on_packet(std::span<const std::uint8_t> packet, const Peer& from,
                       std::chrono::nanoseconds now) {
    if (is_p2_discovery(packet)) {
        // Protocol selection: only Protocol 1 is served, so a client that
        // tries both settles on it.
        if (!p2_discovery_logged_) {
            spdlog::info("Protocol 2 discovery from {} not answered: not served yet",
                         describe(from));
            p2_discovery_logged_ = true;
        }
        return;
    }
    server_.on_packet(packet, from, now);
}

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
    const auto handler = [&](std::span<const std::uint8_t> packet, const Peer& from) {
        on_packet(packet, from, clock());
    };
    while (!stop && !service_.failed()) {
        network_.poll(100, handler);
        session_.tick(clock());
        server_.tick();
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
