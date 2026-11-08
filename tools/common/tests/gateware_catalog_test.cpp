// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/gateware_catalog.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace radioberry;

TEST_CASE("known images are found by hash", "[catalog]") {
    const auto cl025 =
        find_known_image("fe6a4e8acb67fbf80aa679affc8f2e0e0abe15908938cb2aed10804b461105cd");
    REQUIRE(cl025.has_value());
    CHECK(cl025->family == GatewareFamily::kRpi5Gpio);
    CHECK(cl025->fpga == FpgaType::k10CL025);
    CHECK(cl025->size == 334915);

    const auto rpi4 =
        find_known_image("aa5c36f42456bc5dee622a694db2b36aa31dadfbca2372e00e1cd952e7217da3");
    REQUIRE(rpi4.has_value());
    CHECK(rpi4->family == GatewareFamily::kRpi4Gpio);
    CHECK(rpi4->fpga == FpgaType::k10CL016);

    CHECK_FALSE(find_known_image("00").has_value());
}

TEST_CASE("catalog lists the CL025 rpi-5 image first", "[catalog]") {
    const auto images = known_images();
    REQUIRE(images.size() == 3);
    CHECK(images[0].fpga == FpgaType::k10CL025);
    CHECK(images[0].family == GatewareFamily::kRpi5Gpio);
    for (const auto& image : images) {
        CHECK(image.sha256.size() == 64);
        CHECK_FALSE(image.upstream_ref.empty());
    }
}

TEST_CASE("family and FPGA names", "[catalog]") {
    CHECK(to_string(GatewareFamily::kRpi5Pio) == "rpi-5 PIO");
    CHECK(to_string(GatewareFamily::kRpi5Gpio) == "rpi-5 GPIO");
    CHECK(to_string(GatewareFamily::kRpi4Gpio) == "rpi-4 GPIO");
    CHECK(to_string(GatewareFamily::kJuice) == "juice");
    CHECK(to_string(FpgaType::k10CL016) == "10CL016");
    CHECK(to_string(FpgaType::k10CL025) == "10CL025");
}
