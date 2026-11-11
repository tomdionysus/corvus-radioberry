// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/board.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using radioberry::Board;
using radioberry::board_from_compatible;
using radioberry::is_header_gpio_chip;
using radioberry::to_string;

namespace {

// /proc/device-tree/compatible as the kernel presents it.
std::string compatible(std::initializer_list<const char*> entries) {
    std::string out;
    for (const char* e : entries) {
        out += e;
        out += '\0';
    }
    return out;
}

}  // namespace

TEST_CASE("boards are told apart by their SoC", "[board]") {
    CHECK(board_from_compatible(compatible({"raspberrypi,5-model-b", "brcm,bcm2712"})) ==
          Board::kPi5);
    CHECK(board_from_compatible(compatible({"raspberrypi,5-compute-module", "brcm,bcm2712"})) ==
          Board::kPi5);
    CHECK(board_from_compatible(compatible({"raspberrypi,4-model-b", "brcm,bcm2711"})) ==
          Board::kPi4);
    CHECK(board_from_compatible(compatible({"raspberrypi,400", "brcm,bcm2711"})) == Board::kPi4);
    CHECK(board_from_compatible(compatible({"raspberrypi,3-model-b", "brcm,bcm2837"})) ==
          Board::kOther);
    CHECK(board_from_compatible("") == Board::kOther);
    // Without the trailing NUL, and a SoC name that only starts the same.
    CHECK(board_from_compatible(std::string_view("x\0brcm,bcm2711", 14)) == Board::kPi4);
    CHECK(board_from_compatible(std::string_view("x\0brcm,bcm2837", 14)) == Board::kOther);
    CHECK(board_from_compatible(compatible({"brcm,bcm27120"})) == Board::kOther);
}

TEST_CASE("board names", "[board]") {
    CHECK(to_string(Board::kPi5) == "pi5");
    CHECK(to_string(Board::kPi4) == "pi4");
    CHECK(to_string(Board::kOther) == "other");
}

TEST_CASE("the header's GPIO chip on either board", "[board]") {
    CHECK(is_header_gpio_chip("pinctrl-rp1"));
    CHECK(is_header_gpio_chip("pinctrl-bcm2711"));
    CHECK_FALSE(is_header_gpio_chip("pinctrl-bcm2835"));
    CHECK_FALSE(is_header_gpio_chip("raspberrypi-exp-gpio"));
    CHECK_FALSE(is_header_gpio_chip(""));
}
