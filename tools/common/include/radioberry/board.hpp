// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

namespace radioberry {

// The Raspberry Pi boards this project runs on, told apart by their SoC.
// kPi5 covers every BCM2712 board (Pi 5, Pi 500, CM5), kPi4 every BCM2711
// board (Pi 4, Pi 400, CM4).
enum class Board { kPi5, kPi4, kOther };

// The board from /proc/device-tree/compatible: NUL-separated strings, the
// SoC's ("brcm,bcm2712" or "brcm,bcm2711") among them.
Board board_from_compatible(std::string_view compatible);

std::string_view to_string(Board board);

// Whether a GPIO chip with this label carries the 40-pin header's GPIOs,
// as line offsets 0 to 27: pinctrl-rp1 on a Pi 5, pinctrl-bcm2711 on a
// Pi 4.
bool is_header_gpio_chip(std::string_view label);

}  // namespace radioberry
