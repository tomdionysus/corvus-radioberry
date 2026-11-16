// SPDX-License-Identifier: GPL-2.0-or-later
//
// The module's entry points: SoapySDR finds and makes the radio through
// these. Hardware glue, exercised on the Pi rather than by the unit tests.
#include <SoapySDR/Modules.hpp>
#include <SoapySDR/Registry.hpp>
#include <memory>
#include <stdexcept>
#include <system_error>

#include "find.hpp"
#include "radio.hpp"
#include "radioberry/version.hpp"
#include "settings.hpp"

namespace radioberry::soapy {
namespace {

std::string arg(const SoapySDR::Kwargs& args, const char* key, const std::string& otherwise) {
    const auto it = args.find(key);
    return it != args.end() ? it->second : otherwise;
}

SoapySDR::KwargsList find(const SoapySDR::Kwargs& args) { return find_devices(args); }

SoapySDR::Device* make(const SoapySDR::Kwargs& args) {
    const Settings settings = load_settings(arg(args, "config", kConfigPath));
    const std::string path = arg(args, "device", settings.device);
    std::unique_ptr<radioberry::Device> device;
    try {
        device = open_device_nonblocking(path);
    } catch (const std::system_error& e) {
        throw std::runtime_error("radioberry: " + open_error(path, e.code().value()));
    }
    return new Radio(std::move(device), settings);
}

const SoapySDR::Registry kRegistry(kDriverKey, &find, &make, SOAPY_SDR_ABI_VERSION);
const SoapySDR::ModuleVersion kModuleVersion(kVersion);

}  // namespace
}  // namespace radioberry::soapy
