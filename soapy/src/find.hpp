// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SoapySDR/Types.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace radioberry::soapy {

inline constexpr const char* kDriverKey = "radioberry";
// Where the driver publishes the gateware's identity (docs/driver.md).
inline constexpr const char* kSysfsDir = "/sys/bus/spi/devices/spi0.0";

// The radio as SoapySDR lists it, when the device node exists: driver,
// device and a label with the transport and gateware version from
// `sysfs_dir` where it has them. Opens nothing, so a radioberryd holding
// the radio is not disturbed. Nothing for arguments naming another driver
// or a device node that is not there.
SoapySDR::KwargsList find_devices(const SoapySDR::Kwargs& args,
                                  const std::string& sysfs_dir = kSysfsDir);

// The gateware's count of TX samples taken (the `tx_samples` line of the
// driver's `counters` attribute in `sysfs_dir`), or nothing where the
// driver has no such attribute.
std::optional<std::uint32_t> image_tx_samples(const std::string& sysfs_dir = kSysfsDir);

// Why opening the radio at `path` failed with `error` (an errno value),
// and what to do about it.
std::string open_error(const std::string& path, int error);

}  // namespace radioberry::soapy
