// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "radioberry/board.hpp"

namespace radioberry {

// What a gateware image is. This project's two images, one per board, and
// the original RadioBerry software's images, which are recognised only to
// say that they do not work with this driver.
enum class GatewareFamily { kRpi5Pio, kRpi4I2s, kOriginalRpi5, kOriginalRpi4 };

enum class FpgaType { k10CL016, k10CL025 };

struct KnownImage {
    std::string_view sha256;
    GatewareFamily family;
    FpgaType fpga;
    std::size_t size;
    std::string_view path;
    std::string_view source;
};

// Every gateware image recognised by hash, this project's first.
std::span<const KnownImage> known_images();

std::optional<KnownImage> find_known_image(std::string_view sha256);

// The family of the image the driver loads on this board, if any.
std::optional<GatewareFamily> board_family(Board board);

// The file the driver loads on this board: radioberry-<family>.rbf.
std::string_view firmware_name(GatewareFamily family);

std::string_view to_string(GatewareFamily family);
std::string_view to_string(FpgaType fpga);

}  // namespace radioberry
