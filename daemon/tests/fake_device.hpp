// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "radioberry/device.hpp"

namespace radioberry::daemon::test {

// A CL025 behind radioberry.ko, driven by the test. Reads deliver
// `frames_ready` frames, each frame's bytes all equal to its index (mod
// 256) since the device opened. Writes fill a TX ring of
// `tx_capacity_words` that only drain_tx() empties. Every method locks
// `mutex`, so a test changing fields while the service's threads run
// takes it too.
class FakeDevice final : public Device {
public:
    FakeDevice();

    radioberry_info info() override;
    radioberry_stats stats() override;
    Reply command(const Command& command) override;
    std::size_t read(std::span<std::uint8_t> buffer) override;
    std::size_t write(std::span<const std::uint8_t> buffer) override;
    Readiness wait(bool want_read, bool want_write, int timeout_ms) override;

    void drain_tx(std::uint32_t words);

    std::recursive_mutex mutex;
    radioberry_info info_{};
    radioberry_stats stats_{};
    Reply reply;
    std::vector<Command> commands;
    std::size_t frames_ready = 0;
    std::uint64_t frame_index = 0;
    // wait() reports an RX overrun until stats() is next called.
    bool overrun = false;
    // wait() reports readable even with no frames, as after a race.
    bool always_readable = false;
    std::uint32_t tx_capacity_words = 1024;
    std::size_t write_limit = SIZE_MAX;
    std::vector<std::uint8_t> written;
    // The next call of this name ("info", "stats", "command", "read",
    // "write", "wait") throws std::system_error EIO.
    std::string fail_on;
    // With nothing ready, wait() sleeps this long (at most its timeout),
    // so threaded tests do not spin.
    int idle_wait_ms = 0;
    std::size_t waits = 0;
    std::size_t stats_calls = 0;

private:
    void maybe_fail(const char* op);
};

}  // namespace radioberry::daemon::test
