// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/fpga_config.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace radioberry;

namespace {

// Simulates a Cyclone passive serial port: captures bits on rising DCLK
// edges, asserts CONF_DONE once the expected bitstream has been clocked in.
class SimulatedFpga final : public ConfigPins {
public:
    explicit SimulatedFpga(std::vector<std::uint8_t> expected) : expected_(std::move(expected)) {}

    void set_nconfig(bool high) override {
        if (!high) {
            nstatus_ = stuck_nstatus_high_;
            received_.clear();
            current_ = 0;
            bit_ = 0;
            configuring_ = false;
        } else if (!stuck_nstatus_low_) {
            nstatus_ = true;
            configuring_ = true;
        }
    }
    void set_data(bool high) override { data_ = high; }
    void set_dclk(bool high) override {
        if (high && !dclk_) {
            ++rising_edges_;
            if (configuring_ && !conf_done_) {
                current_ = static_cast<std::uint8_t>(current_ | (data_ ? (1U << bit_) : 0U));
                if (++bit_ == 8) {
                    received_.push_back(current_);
                    current_ = 0;
                    bit_ = 0;
                    if (received_.size() == expected_.size()) {
                        if (received_ == expected_) {
                            conf_done_ = true;
                        } else if (crc_on_mismatch_) {
                            nstatus_ = false;
                        }
                    }
                }
            }
        }
        dclk_ = high;
    }
    bool nstatus() override { return nstatus_; }
    bool conf_done() override { return conf_done_; }
    void delay(std::chrono::microseconds duration) override { waited_ += duration; }

    bool stuck_nstatus_high_ = false;
    bool stuck_nstatus_low_ = false;
    bool crc_on_mismatch_ = false;
    std::vector<std::uint8_t> received_;
    long rising_edges_ = 0;
    std::chrono::microseconds waited_{0};

private:
    std::vector<std::uint8_t> expected_;
    bool nstatus_ = true;
    bool conf_done_ = false;
    bool configuring_ = false;
    bool data_ = false;
    bool dclk_ = false;
    std::uint8_t current_ = 0;
    int bit_ = 0;
};

}  // namespace

TEST_CASE("passive serial clocks the image least significant bit first", "[fpga]") {
    const std::vector<std::uint8_t> image = {0x01, 0x80, 0xa5, 0xff, 0x00, 0x3c};
    SimulatedFpga fpga(image);
    const auto result = configure_passive_serial(fpga, image);
    CHECK(result.ok());
    CHECK(result.message() == "configured");
    CHECK(fpga.received_ == image);
    CHECK(fpga.conf_done());
    // Image bits plus the initialisation clocks.
    CHECK(fpga.rising_edges_ == static_cast<long>(image.size() * 8 + 64));
}

TEST_CASE("an image for another FPGA leaves CONF_DONE low", "[fpga]") {
    SimulatedFpga fpga({0x11, 0x22, 0x33});
    const std::vector<std::uint8_t> wrong = {0x11, 0x22, 0x34};
    const auto result = configure_passive_serial(fpga, wrong);
    CHECK(result.error == ConfigError::kConfDoneLow);
    CHECK(result.message().find("CONF_DONE") != std::string::npos);
}

TEST_CASE("a rejected bitstream reports the nSTATUS error", "[fpga]") {
    SimulatedFpga fpga({0x11, 0x22});
    fpga.crc_on_mismatch_ = true;
    const std::vector<std::uint8_t> wrong = {0x11, 0x23};
    const auto result = configure_passive_serial(fpga, wrong);
    CHECK(result.error == ConfigError::kCrcError);
    CHECK(result.message().find("rejected") != std::string::npos);
}

TEST_CASE("nSTATUS that never follows nCONFIG low is reported", "[fpga]") {
    SimulatedFpga fpga({0x00});
    fpga.stuck_nstatus_high_ = true;
    const auto result = configure_passive_serial(fpga, std::vector<std::uint8_t>{0x00});
    CHECK(result.error == ConfigError::kNStatusStuckHigh);
    CHECK(result.message().find("stayed high") != std::string::npos);
    CHECK(fpga.waited_.count() > 0);
}

TEST_CASE("nSTATUS that never releases is reported", "[fpga]") {
    SimulatedFpga fpga({0x00});
    fpga.stuck_nstatus_low_ = true;
    const auto result = configure_passive_serial(fpga, std::vector<std::uint8_t>{0x00});
    CHECK(result.error == ConfigError::kNStatusStuckLow);
    CHECK(result.message().find("stayed low") != std::string::npos);
}
