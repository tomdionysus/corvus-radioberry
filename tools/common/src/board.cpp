// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/board.hpp"

namespace radioberry {

Board board_from_compatible(std::string_view compatible) {
    while (!compatible.empty()) {
        const auto end = compatible.find('\0');
        const std::string_view entry = compatible.substr(0, end);
        if (entry == "brcm,bcm2712") {
            return Board::kPi5;
        }
        if (entry == "brcm,bcm2711") {
            return Board::kPi4;
        }
        if (end == std::string_view::npos) {
            break;
        }
        compatible.remove_prefix(end + 1);
    }
    return Board::kOther;
}

std::string_view to_string(Board board) {
    switch (board) {
        case Board::kPi5:
            return "pi5";
        case Board::kPi4:
            return "pi4";
        case Board::kOther:
            break;
    }
    return "other";
}

bool is_header_gpio_chip(std::string_view label) {
    return label == "pinctrl-rp1" || label == "pinctrl-bcm2711";
}

}  // namespace radioberry
