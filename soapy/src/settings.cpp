// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.hpp"

#include <filesystem>

#include "options.hpp"

namespace radioberry::soapy {

Settings load_settings(const std::string& path) {
    Settings s;
    s.path = path;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return s;
    }
    const daemon::Options o = daemon::parse_options({"--config", path});
    s.device = o.device;
    s.frequency_ppm = o.daemon.frequency_ppm;
    s.lna_gain_db = o.daemon.p2_rx_gain_db;
    s.tx = o.daemon.tx;
    s.from_file = true;
    return s;
}

}  // namespace radioberry::soapy
