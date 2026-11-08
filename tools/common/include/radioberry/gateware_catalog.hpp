// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace radioberry {

// Which host interface a gateware image was built for. Only the rpi-5 PIO
// family works on a Raspberry Pi 5.
enum class GatewareFamily { kRpi5Pio, kRpi4Gpio, kJuice };

enum class FpgaType { k10CL016, k10CL025 };

struct KnownImage {
    std::string_view sha256;
    GatewareFamily family;
    FpgaType fpga;
    std::size_t size;
    std::string_view upstream_path;
    std::string_view upstream_ref;
};

// Every upstream image this project recognises by hash.
std::span<const KnownImage> known_images();

std::optional<KnownImage> find_known_image(std::string_view sha256);

std::string_view to_string(GatewareFamily family);
std::string_view to_string(FpgaType fpga);

}  // namespace radioberry
