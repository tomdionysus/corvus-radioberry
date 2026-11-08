// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <span>
#include <string>
#include <vector>

#include "radioberry/gateware_catalog.hpp"
#include "radioberry/node.hpp"
#include "source.hpp"

namespace radioberry::probe {

enum class Severity { kBlocker, kWarning, kInfo };

struct Finding {
    Severity severity;
    std::string id;
    std::string message;
};

struct Options {
    // Identify the FPGA by configuring it with each known rpi-5 image in
    // turn. Changes FPGA state; refused while the driver is loaded.
    bool identify = false;
    std::vector<std::string> gateware_dirs = {"/usr/local/share/radioberry/gateware",
                                              "/usr/share/radioberry/gateware", "/lib/firmware"};
    // Images recognised by hash; tests substitute their own.
    std::span<const KnownImage> catalog = known_images();
};

struct Report {
    Node root;
    std::vector<Finding> findings;

    bool radio_can_run() const;
};

std::string_view to_string(Severity severity);

Report collect(Source& source, const Options& options);

}  // namespace radioberry::probe
