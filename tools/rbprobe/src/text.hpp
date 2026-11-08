// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace radioberry::probe {

// Strips surrounding whitespace and NUL bytes (device tree strings end in
// NUL).
std::string trim(std::string_view text);
std::vector<std::string> split_lines(std::string_view text);
std::vector<std::string> split_words(std::string_view text);
std::string base_name(std::string_view path);
// Value of KEY=value in a shell-style assignment file, or empty.
std::string key_value(std::string_view text, std::string_view key);
std::string unquote(std::string_view text);

}  // namespace radioberry::probe
