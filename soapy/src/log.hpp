// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <spdlog/common.h>

#include <SoapySDR/Logger.hpp>
#include <string>

namespace radioberry::soapy {

// The SoapySDR log level for an spdlog level.
SoapySDRLogLevel soapy_level(spdlog::level::level_enum level);

// Sends the messages of the radioberryd code the module reuses (written
// with spdlog) to SoapySDR's logger, leaving out those that begin with
// `drop_prefix` (the session's ownership messages, which name a network
// client that does not exist here). The module carries its own copy of
// spdlog, so this is not the host program's spdlog logger. Once per
// process; later calls change nothing.
void install_logger(const std::string& drop_prefix);

}  // namespace radioberry::soapy
