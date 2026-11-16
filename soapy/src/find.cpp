// SPDX-License-Identifier: GPL-2.0-or-later
#include "find.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "radioberry/device.hpp"

namespace radioberry::soapy {
namespace {

// The first line of a sysfs attribute, or empty.
std::string attribute(const std::string& dir, const char* name) {
    std::ifstream in(dir + "/" + name);
    std::string line;
    std::getline(in, line);
    return line;
}

}  // namespace

SoapySDR::KwargsList find_devices(const SoapySDR::Kwargs& args, const std::string& sysfs_dir) {
    if (const auto d = args.find("driver"); d != args.end() && d->second != kDriverKey) {
        return {};
    }
    const auto p = args.find("device");
    const std::string path = p != args.end() ? p->second : kDevicePath;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }
    SoapySDR::Kwargs found;
    found["driver"] = kDriverKey;
    found["device"] = path;
    std::string label = "RadioBerry";
    const std::string transport = attribute(sysfs_dir, "transport");
    const std::string gateware = attribute(sysfs_dir, "gateware_version");
    if (!transport.empty() && !gateware.empty()) {
        label += " (" + transport + ", gateware " + gateware + ")";
    }
    found["label"] = label;
    return {found};
}

std::optional<std::uint32_t> image_tx_samples(const std::string& sysfs_dir) {
    std::ifstream in(sysfs_dir + "/counters");
    std::string line;
    // "tx_samples 0xHEX decimal" among other lines.
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string name;
        std::string hex;
        std::uint64_t value = 0;
        if (fields >> name >> hex >> value && name == "tx_samples") {
            return static_cast<std::uint32_t>(value);
        }
    }
    return std::nullopt;
}

std::string open_error(const std::string& path, int error) {
    std::string text = path + ": " + std::generic_category().message(error);
    switch (error) {
        case EBUSY:
            return text +
                   ": another program has the radio, usually radioberryd; stop it with "
                   "'sudo systemctl stop radioberry'";
        case EACCES:
        case EPERM:
            return text +
                   ": add the user to the radioberry group ('sudo usermod -aG radioberry "
                   "$USER') and log in again";
        case ENOENT:
        case ENODEV:
        case ENXIO:
            return text +
                   ": is the radioberry driver loaded, with dtoverlay=radioberry in "
                   "/boot/firmware/config.txt?";
        default:
            return text;
    }
}

}  // namespace radioberry::soapy
