// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/gateware_catalog.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace radioberry;

TEST_CASE("the catalog lists this project's two images first, then the originals",
          "[catalog]") {
    const auto images = known_images();
    REQUIRE(images.size() == 5);
    CHECK(images[0].family == GatewareFamily::kRpi5Pio);
    CHECK(images[1].family == GatewareFamily::kRpi4I2s);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(images[i].fpga == FpgaType::k10CL025);
        CHECK(images[i].size > 100000);
    }
    CHECK(images[2].family == GatewareFamily::kOriginalRpi5);
    CHECK(images[4].family == GatewareFamily::kOriginalRpi4);
    for (const auto& image : images) {
        CHECK(image.sha256.size() == 64);
        CHECK_FALSE(image.source.empty());
        CHECK_FALSE(image.path.empty());
    }
}

TEST_CASE("known images are found by hash", "[catalog]") {
    const auto images = known_images();
    const auto pi5 = find_known_image(images[0].sha256);
    REQUIRE(pi5.has_value());
    CHECK(pi5->family == GatewareFamily::kRpi5Pio);

    const auto original =
        find_known_image("aa5c36f42456bc5dee622a694db2b36aa31dadfbca2372e00e1cd952e7217da3");
    REQUIRE(original.has_value());
    CHECK(original->family == GatewareFamily::kOriginalRpi4);
    CHECK(original->fpga == FpgaType::k10CL016);

    CHECK_FALSE(find_known_image("00").has_value());
}

TEST_CASE("each board has its image and file name", "[catalog]") {
    CHECK(board_family(Board::kPi5) == GatewareFamily::kRpi5Pio);
    CHECK(board_family(Board::kPi4) == GatewareFamily::kRpi4I2s);
    CHECK_FALSE(board_family(Board::kOther).has_value());
    CHECK(firmware_name(GatewareFamily::kRpi5Pio) == "radioberry-rpi5-pio.rbf");
    CHECK(firmware_name(GatewareFamily::kRpi4I2s) == "radioberry-rpi4-i2s.rbf");
    CHECK(firmware_name(GatewareFamily::kOriginalRpi5) == "radioberry.rbf");
    CHECK(firmware_name(GatewareFamily::kOriginalRpi4) == "radioberry.rbf");
}

TEST_CASE("family and FPGA names", "[catalog]") {
    CHECK(to_string(GatewareFamily::kRpi5Pio) == "rpi5-pio");
    CHECK(to_string(GatewareFamily::kRpi4I2s) == "rpi4-i2s");
    CHECK(to_string(GatewareFamily::kOriginalRpi5) == "original rpi-5");
    CHECK(to_string(GatewareFamily::kOriginalRpi4) == "original rpi-4");
    CHECK(to_string(static_cast<GatewareFamily>(99)) == "unknown");
    CHECK(firmware_name(static_cast<GatewareFamily>(99)) == "radioberry.rbf");
    CHECK(to_string(FpgaType::k10CL016) == "10CL016");
    CHECK(to_string(FpgaType::k10CL025) == "10CL025");
    CHECK(to_string(static_cast<FpgaType>(99)) == "unknown");
}
