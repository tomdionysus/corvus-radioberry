// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/gateware_catalog.hpp"

#include <array>

namespace radioberry {
namespace {

constexpr std::string_view kTag = "radioberry@rpi5 (269e402, 2025-12-16)";

// Ordered so that the image for the development board's FPGA comes first;
// identification by configuration tries them in this order.
constexpr std::array<KnownImage, 3> kImages = {{
    {"fe6a4e8acb67fbf80aa679affc8f2e0e0abe15908938cb2aed10804b461105cd", GatewareFamily::kRpi5Pio,
     FpgaType::k10CL025, 334915, "SBC/rpi-5/releases/dev/CL025/radioberry.rbf", kTag},
    {"5f146a8c2113a888bd6b7d08d8ac9f62b796e79dd950ecd47b31b5ec1ebe9e48", GatewareFamily::kRpi5Pio,
     FpgaType::k10CL016, 275438, "SBC/rpi-5/releases/dev/CL016/radioberry.rbf", kTag},
    {"aa5c36f42456bc5dee622a694db2b36aa31dadfbca2372e00e1cd952e7217da3", GatewareFamily::kRpi4Gpio,
     FpgaType::k10CL016, 276541, "SBC/rpi-4/releases/dev/CL016/radioberry.rbf", kTag},
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

std::string_view to_string(GatewareFamily family) {
    switch (family) {
        case GatewareFamily::kRpi5Pio:
            return "rpi-5 PIO";
        case GatewareFamily::kRpi4Gpio:
            return "rpi-4 GPIO";
        case GatewareFamily::kJuice:
            return "juice";
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
