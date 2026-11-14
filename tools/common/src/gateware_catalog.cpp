// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/gateware_catalog.hpp"

#include <array>

namespace radioberry {
namespace {

constexpr std::string_view kThis = "this project (gateware/docs/provenance.md)";
constexpr std::string_view kOriginal = "Radioberry-2.x radioberry@rpi5 (269e402), gateware 73.3";

// RB_IMAGE_*: from the committed images, by tools/common/CMakeLists.txt.
constexpr std::array<KnownImage, 5> kImages = {{
    {RB_IMAGE_RPI5_PIO_SHA256, GatewareFamily::kRpi5Pio, FpgaType::k10CL025, RB_IMAGE_RPI5_PIO_SIZE,
     "gateware/images/rpi5-pio/CL025/radioberry-rpi5-pio.rbf", kThis},
    {RB_IMAGE_RPI4_I2S_SHA256, GatewareFamily::kRpi4I2s, FpgaType::k10CL025, RB_IMAGE_RPI4_I2S_SIZE,
     "gateware/images/rpi4-i2s/CL025/radioberry-rpi4-i2s.rbf", kThis},
    {"fe6a4e8acb67fbf80aa679affc8f2e0e0abe15908938cb2aed10804b461105cd",
     GatewareFamily::kOriginalRpi5, FpgaType::k10CL025, 334915,
     "SBC/rpi-5/archive/releases/dev/CL025/radioberry.rbf", kOriginal},
    {"5f146a8c2113a888bd6b7d08d8ac9f62b796e79dd950ecd47b31b5ec1ebe9e48",
     GatewareFamily::kOriginalRpi5, FpgaType::k10CL016, 275438,
     "SBC/rpi-5/archive/releases/dev/CL016/radioberry.rbf", kOriginal},
    {"aa5c36f42456bc5dee622a694db2b36aa31dadfbca2372e00e1cd952e7217da3",
     GatewareFamily::kOriginalRpi4, FpgaType::k10CL016, 276541,
     "SBC/rpi-4/releases/dev/CL016/radioberry.rbf", kOriginal},
}};

}  // namespace

std::span<const KnownImage> known_images() { return kImages; }

std::optional<KnownImage> find_known_image(std::string_view sha256) {
    for (const auto& image : kImages) {
        if (image.sha256 == sha256) {
            return image;
        }
    }
    return std::nullopt;
}

std::optional<GatewareFamily> board_family(Board board) {
    switch (board) {
        case Board::kPi5:
            return GatewareFamily::kRpi5Pio;
        case Board::kPi4:
            return GatewareFamily::kRpi4I2s;
        case Board::kOther:
            break;
    }
    return std::nullopt;
}

std::string_view firmware_name(GatewareFamily family) {
    switch (family) {
        case GatewareFamily::kRpi5Pio:
            return "radioberry-rpi5-pio.rbf";
        case GatewareFamily::kRpi4I2s:
            return "radioberry-rpi4-i2s.rbf";
        case GatewareFamily::kOriginalRpi5:
        case GatewareFamily::kOriginalRpi4:
            break;
    }
    return "radioberry.rbf";
}

std::string_view to_string(GatewareFamily family) {
    switch (family) {
        case GatewareFamily::kRpi5Pio:
            return "rpi5-pio";
        case GatewareFamily::kRpi4I2s:
            return "rpi4-i2s";
        case GatewareFamily::kOriginalRpi5:
            return "original rpi-5";
        case GatewareFamily::kOriginalRpi4:
            return "original rpi-4";
    }
    return "unknown";
}

std::string_view to_string(FpgaType fpga) {
    switch (fpga) {
        case FpgaType::k10CL016:
            return "10CL016";
        case FpgaType::k10CL025:
            return "10CL025";
    }
    return "unknown";
}

}  // namespace radioberry
