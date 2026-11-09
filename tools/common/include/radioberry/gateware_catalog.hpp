// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace radioberry {

// Which host interface a gateware image was built for. Only the rpi-5 PIO
// family works with this project's driver; rpi-5 GPIO is the older 73.3
// gateware with an 8-bit RX bus.
enum class GatewareFamily { kRpi5Pio, kRpi5Gpio, kRpi4Gpio, kJuice };

enum class FpgaType { k10CL016, k10CL025 };

struct KnownImage {
    std::string_view sha256;
    GatewareFamily family;
    FpgaType fpga;
    std::size_t size;
    std::string_view path;
    std::string_view source;
};

// Every gateware image this project recognises by hash: its own builds and
// the original RadioBerry software's images.
std::span<const KnownImage> known_images();

std::optional<KnownImage> find_known_image(std::string_view sha256);

std::string_view to_string(GatewareFamily family);
std::string_view to_string(FpgaType fpga);

}  // namespace radioberry
