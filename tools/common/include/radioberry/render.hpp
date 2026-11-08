// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "radioberry/node.hpp"

namespace radioberry {

// Human text: one section per top-level key, aligned key/value lines,
// sequences of maps one line per item.
std::string render_text(const Node& root);
std::string render_json(const Node& root);
std::string render_yaml(const Node& root);
// One scalar as text: "-" for null, "yes"/"no" for booleans.
std::string scalar_text(const Node& node);

}  // namespace radioberry
