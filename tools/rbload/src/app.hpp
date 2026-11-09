// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <variant>

#include "radioberry/fpga_config.hpp"
#include "radioberry/gateware_catalog.hpp"
#include "radioberry/node.hpp"

namespace radioberry::load {

enum ExitCode : int {
    kExitOk = 0,
    kExitFailed = 1,
    kExitUsage = 2,
};

enum class ReportFormat { kText, kJson, kYaml };

inline constexpr const char* kDefaultImage = "/lib/firmware/radioberry.rbf";

struct Options {
    std::string image = kDefaultImage;
    // The FPGA the image must be for, when given.
    std::optional<FpgaType> fpga;
    // Load an image that is not a known rpi-5 PIO build.
    bool force = false;
    ReportFormat format = ReportFormat::kText;
};

struct ShowHelp {};
struct ShowVersion {};
struct UsageError {
    std::string message;
};

using ParseResult = std::variant<Options, ShowHelp, ShowVersion, UsageError>;

ParseResult parse_options(std::span<const std::string> args);

// Why an image with this hash may not be loaded under `options`, or
// nothing: not a known image, a known image of another family than the
// rpi-5 PIO builds the driver needs, or one for another FPGA than --fpga.
std::optional<std::string> refusal(const Options& options, const std::optional<KnownImage>& known);

struct Environment {
    // The file's contents; throws std::runtime_error if it cannot be read.
    std::function<std::string(const std::string& path)> read_file;
    // True while radioberry.ko is loaded: it owns the configuration lines.
    std::function<bool()> driver_loaded;
    std::function<std::unique_ptr<ConfigPins>()> open_pins;
    std::ostream& out;
    std::ostream& err;
};

int run_app(std::span<const std::string> args, Environment& env);

extern const char* const kUsage;

}  // namespace radioberry::load
