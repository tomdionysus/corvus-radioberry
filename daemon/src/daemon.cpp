// SPDX-License-Identifier: GPL-2.0-or-later
#include "daemon.hpp"

#include <spdlog/spdlog.h>

#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "device/capacity.hpp"
#include "realtime.hpp"

namespace radioberry::daemon {
namespace {

p1::Identity identify(Device& device, const std::optional<std::array<std::uint8_t, 6>>& mac) {
    const radioberry_info info = device.info();
    p1::Identity id;
    id.mac = mac ? *mac : interface_mac().value_or(std::array<std::uint8_t, 6>{});
    id.version_major = info.gateware_major;
    id.version_minor = info.gateware_minor;
    id.receivers = info.receivers;
    id.rx_values_max = info.rx_values_max;
    return id;
}

// The device options with the hook that sets each device thread's
// scheduling.
DeviceOptions with_thread_hook(DeviceOptions options, std::function<void(const char*)> hook) {
    options.on_thread_start = std::move(hook);
    return options;
}

// Faults that break the transmit stream: the session drops MOX on them.
// A driver TX underrun is not one: the driver's ring runs empty for moments
// whenever the gateware fills its own 85 ms FIFO at PIO speed (on PTT, at
// about 102 000 samples per second), and the ring is kept short for low
// latency, so it counts underruns while the gateware's FIFO is still full.
// Real starvation ends transmission through the gateware's own PTT
// hang-time watchdog, the TX watchdog, the client timeout and the other
// faults here.
bool breaks_transmit(FaultKind kind) {
    switch (kind) {
        case FaultKind::TxInvalidBlock:
        case FaultKind::RxQueueFull:
        case FaultKind::DeviceError:
            return true;
        default:
            return false;
    }
}

}  // namespace

Daemon::Daemon(Device& device, const DaemonOptions& options)
    : identity_(identify(device, options.mac)),
      session_([this] { service_.control().safe(); }, options.client_timeout),
      service_(
          device, [this](const Fault& fault) { on_fault(fault); },
          with_thread_hook(options.device,
                           [this](const char* role) {
                               start_thread(
                                   std::string(role) == "rx" ? "rb-rx-device" : "rb-tx-device",
                                   kDevicePriority);
                           })),
      policy_(service_.control(), session_, options.tx),
      network_(options.address, options.port),
      server_(
          identity_, service_.control(), service_.tx(), session_, policy_,
          [this] {
              const DeviceSnapshot s = service_.snapshot();
              return p1::GatewareStatus{s.cw_or_ptt, s.external_cw_key};
          },
          network_),
      p2_network_(options.address, options.port, identity_.receivers,
                  [this](const Peer& to, std::span<const std::uint8_t> bytes) {
                      network_.send(to, bytes);
                  }),
      p2_server_(
          p2::Identity{identity_.mac, identity_.version_major, identity_.receivers,
                       identity_.rx_values_max},
          service_.control(), service_.tx(), session_, policy_,
          [this] {
              const DeviceSnapshot s = service_.snapshot();
              return p1::GatewareStatus{s.cw_or_ptt, s.external_cw_key};
          },
          p2_network_) {
    server_.set_frequency_ppm(options.frequency_ppm);
    server_.set_iq_convention(options.iq_convention);
    p2_server_.set_frequency_ppm(options.frequency_ppm);
    p2_server_.set_iq_convention(options.iq_convention);
    p2_server_.set_rx_gain_db(options.p2_rx_gain_db);
    iq_standard_ = options.iq_convention == p1::IqConvention::Standard;
    frequency_ppm_ = options.frequency_ppm;
    realtime_ = options.realtime;
    protocol1_ = options.protocol1;
    protocol2_ = options.protocol2;
}

void Daemon::start_thread(const char* name, int priority, bool rename) {
    if (rename) {
        name_thread(name);
    }
    if (!realtime_) {
        return;
    }
    if (const auto failed = make_thread_realtime(name, priority);
        failed && !realtime_failed_.exchange(true)) {
        spdlog::warn("real-time scheduling not set: {}", *failed);
    }
}

Daemon::~Daemon() = default;

void Daemon::on_fault(const Fault& fault) {
    if (fault.kind == FaultKind::DeviceError) {
        spdlog::error("{}", describe(fault));
    } else if (fault.kind == FaultKind::TxUnderrun) {
        spdlog::debug("{}", describe(fault));  // summarised per transmission
    } else {
        spdlog::warn("{}", describe(fault));
    }
    if (breaks_transmit(fault.kind)) {
        session_.fault(describe(fault));
    }
}

void Daemon::update_run() {
    const std::lock_guard lock(run_mutex_);
    service_.control().set_run(server_.running() || p2_server_.running());
}

void Daemon::on_packet(std::span<const std::uint8_t> packet, const Peer& from,
                       std::chrono::nanoseconds now) {
    // Protocol 2 shares the base port for discovery and General packets;
    // Protocol 1 packets start EF FE.
    if (!from.tcp && (is_p2_discovery(packet) || p2::is_general(packet))) {
        if (protocol2_) {
            p2_server_.on_packet(p2::kGeneralPort, packet, from, now);
            update_run();
        }
        return;
    }
    if (protocol1_) {
        server_.on_packet(packet, from, now);
        update_run();
    }
}

bool Daemon::run(const std::atomic<bool>& stop) {
    const TxPolicyOptions& tx = policy_.options();
    spdlog::info("gateware {}.{}, {} receivers", identity_.version_major, identity_.version_minor,
                 identity_.receivers);
    if (protocol1_) {
        spdlog::info("Protocol 1 on port {}", port());
    } else {
        spdlog::info("Protocol 1 off");
    }
    if (protocol2_) {
        spdlog::info("Protocol 2 on ports {} to {} and {} to {}", p2_port(p2::kGeneralPort),
                     p2_port(p2::kTxIqPort), p2_port(p2::kDdcPort0),
                     p2_port(p2::kDdcPort0 + identity_.receivers - 1));
    } else {
        spdlog::info("Protocol 2 off");
    }
    if (identity_.rx_values_max != 0 && identity_.rx_values_max < kRxValuesPerSecondMax) {
        spdlog::info(
            "the driver carries up to {} RX values per second: {} receiver(s) at 48 ksps, "
            "{} at 192 ksps",
            identity_.rx_values_max, max_receivers(0, identity_.receivers, identity_.rx_values_max),
            max_receivers(2, identity_.receivers, identity_.rx_values_max));
    }
    if (iq_standard_) {
        spdlog::info("I/Q in the standard convention: I and Q swapped both ways");
    }
    if (frequency_ppm_ != 0) {
        spdlog::info("frequencies corrected for a radio error of {:+.2f} ppm", frequency_ppm_);
    }
    if (realtime_) {
        if (const auto failed = lock_memory()) {
            spdlog::warn("{}", *failed);
        }
        spdlog::info("real-time: SCHED_FIFO {} for the device threads, {} for the network threads",
                     kDevicePriority, kNetworkPriority);
    }
    start_thread("the Protocol 1 network thread", kNetworkPriority, false);
    if (tx.allow_tx) {
        spdlog::warn(
            "transmit allowed: drive ceiling {}, PA {}, watchdog {} s; no PA interlock is claimed",
            tx.drive_ceiling, tx.allow_pa ? "allowed" : "not allowed",
            std::chrono::duration_cast<std::chrono::seconds>(tx.watchdog).count());
    } else {
        spdlog::info("transmit not allowed (--allow-tx): receive only");
    }
    // The RX delivery thread runs before the device does, so the first
    // blocks find it draining the ring.
    std::atomic<bool> stop_rx{false};
    std::thread rx([&] {
        start_thread("rb-rx-send", kNetworkPriority);
        while (!stop_rx) {
            service_.rx().wait(stop_rx);
            while (const RxBlock* block = service_.rx().front()) {
                server_.on_rx(*block);
                p2_server_.on_rx(*block);
                service_.rx().pop();
            }
        }
    });
    if (!service_.start()) {
        stop_rx = true;
        service_.rx().wake();
        rx.join();
        return false;
    }
    const Clock clock = steady_clock();
    std::atomic<bool> stop_p2{false};
    std::thread p2([&] {
        start_thread("rb-p2-network", kNetworkPriority);
        const auto p2_handler = [&](unsigned port, std::span<const std::uint8_t> packet,
                                    const Peer& from) {
            if (protocol2_) {
                p2_server_.on_packet(port, packet, from, clock());
                update_run();
            }
        };
        while (!stop_p2) {
            p2_network_.poll(100, p2_handler);
        }
    });
    const auto handler = [&](std::span<const std::uint8_t> packet, const Peer& from) {
        on_packet(packet, from, clock());
    };
    // Each transmission summarised from the driver's statistics.
    bool was_transmitting = false;
    std::chrono::nanoseconds tx_started{};
    radioberry_stats tx_before{};
    std::uint64_t dropped_before = 0;
    // Failed sends, reported at most every 10 s with the count since.
    std::uint64_t send_failures_reported = 0;
    std::chrono::nanoseconds send_failures_at{};
    while (!stop && !service_.failed()) {
        network_.poll(100, handler);
        const auto now = clock();
        const std::uint64_t send_failures = network_.send_failures() + p2_network_.send_failures();
        if (send_failures != send_failures_reported &&
            now - send_failures_at >= std::chrono::seconds(10)) {
            spdlog::warn("{} sends to clients failed", send_failures - send_failures_reported);
            send_failures_reported = send_failures;
            send_failures_at = now;
        }
        session_.tick(now);
        policy_.tick(now);
        server_.tick();
        p2_server_.tick();
        update_run();  // a client timed out or lost the radio
        const bool transmitting = policy_.transmitting();
        if (transmitting != was_transmitting) {
            const DeviceSnapshot s = service_.snapshot();
            const radioberry_stats driver = s.driver.value_or(radioberry_stats{});
            if (transmitting) {
                tx_started = now;
                tx_before = driver;
                dropped_before = s.tx_blocks_dropped;
            } else {
                const double seconds = std::chrono::duration<double>(now - tx_started).count();
                const auto words = driver.tx_words - tx_before.tx_words;
                // Driver TX underruns are left out: they count every moment
                // the driver's ring empties between client bursts, not the
                // gateware's FIFO running dry (see breaks_transmit).
                spdlog::info(
                    "transmission of {:.1f} s: {} samples to the gateware ({:.0f} per second), "
                    "{} TX blocks dropped",
                    seconds, words, seconds > 0 ? static_cast<double>(words) / seconds : 0.0,
                    s.tx_blocks_dropped - dropped_before);
            }
            was_transmitting = transmitting;
        }
    }
    const bool failed = service_.failed();
    stop_p2 = true;
    p2.join();
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
