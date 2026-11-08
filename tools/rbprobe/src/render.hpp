// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "node.hpp"

namespace radioberry::probe {

std::string render_text(const Node& root);
std::string render_json(const Node& root);
std::string render_yaml(const Node& root);

}  // namespace radioberry::probe
