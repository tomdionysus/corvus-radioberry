// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

#include "device/service.hpp"
#include "net.hpp"
#include "p1/server.hpp"
#include "radioberry/device.hpp"

namespace radioberry::daemon {

struct DaemonOptions {
    std::uint32_t address = 0;  // host byte order; 0 is every interface
    std::uint16_t port = p1::kPort;
    std::optional<std::array<std::uint8_t, 6>> mac;  // default: interface_mac()
    DeviceOptions device;
    p1::ServerOptions server;
};

// radioberryd: the device layer, the Protocol 1 server and its sockets.
// The calling thread runs the network; a second thread hands RX blocks to
// the server, which streams them to the client.
class Daemon {
public:
    // Reads the gateware identity and binds the sockets; throws
    // std::system_error when either fails.
    Daemon(Device& device, DaemonOptions options);
    ~Daemon();
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    // Runs until `stop` is set or the device fails, then leaves the
    // gateware in the safe state. False if the device failed.
    bool run(const std::atomic<bool>& stop);

    std::uint16_t port() const { return network_.port(); }
    const p1::Identity& identity() const { return identity_; }
    p1::Server& server() { return server_; }
    DeviceService& device() { return service_; }

private:
    p1::Identity identity_;
    DeviceService service_;
    Network network_;
    p1::Server server_;
};

}  // namespace radioberry::daemon
