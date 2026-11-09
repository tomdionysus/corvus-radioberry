// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <chrono>
#include <cmath>
#include <exception>

#include "radioberry/render.hpp"
#include "radioberry/sha256.hpp"
#include "radioberry/version.hpp"

namespace radioberry::load {

const char* const kUsage =
    "usage: rbload [options] [IMAGE]\n"
    "\n"
    "Loads a gateware image into the Radioberry's FPGA through its passive\n"
    "serial configuration lines, without the kernel driver, and checks that\n"
    "nSTATUS and CONF_DONE report success. IMAGE defaults to\n"
    "/lib/firmware/radioberry.rbf. Only images rbprobe knows as this\n"
    "project's rpi-5 PIO builds are loaded unless --force is given. Exits 0\n"
    "when the FPGA configured, 1 when it did not or the image cannot be read,\n"
    "2 on a usage error or a refused image.\n"
    "\n"
    "  --fpga CL025|CL016  refuse an image for the other FPGA\n"
    "  --force             load an image that is not a known rpi-5 PIO build\n"
    "  --json              JSON output\n"
    "  --yaml              YAML output\n"
    "  --version           print the version\n"
    "  --help              this text\n"
    "\n"
    "The driver owns the configuration lines while it is loaded, so rbload\n"
    "refuses to run until it is unloaded (rmmod radioberry, after stopping\n"
    "radioberry.service). To keep the loaded image when the driver comes\n"
    "back, load it with load_gateware=0.\n";

namespace {

std::optional<FpgaType> parse_fpga(const std::string& text) {
    if (text == "CL025" || text == "10CL025") {
        return FpgaType::k10CL025;
    }
    if (text == "CL016" || text == "10CL016") {
        return FpgaType::k10CL016;
    }
    return std::nullopt;
}

Node image_node(const Options& options, std::size_t bytes, const std::string& sha256,
                const std::optional<KnownImage>& known) {
    Node n = Node::map();
    n.set("path", options.image);
    n.set("bytes", bytes);
    n.set("sha256", sha256);
    n.set("known", known.has_value());
    if (known) {
        n.set("family", std::string(to_string(known->family)));
        n.set("fpga", std::string(to_string(known->fpga)));
        n.set("source", std::string(known->upstream_ref));
    } else {
        n.set("family", Node());
        n.set("fpga", Node());
        n.set("source", Node());
    }
    return n;
}

void render(const Options& options, const Node& report, std::ostream& out) {
    switch (options.format) {
        case ReportFormat::kText:
            out << render_text(report);
            break;
        case ReportFormat::kJson:
            out << render_json(report);
            break;
        case ReportFormat::kYaml:
            out << render_yaml(report);
            break;
    }
}

}  // namespace

ParseResult parse_options(std::span<const std::string> args) {
    Options o;
    bool image_given = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            return ShowHelp{};
        }
        if (arg == "--version") {
            return ShowVersion{};
        }
        if (arg == "--json") {
            o.format = ReportFormat::kJson;
        } else if (arg == "--yaml") {
            o.format = ReportFormat::kYaml;
        } else if (arg == "--force") {
            o.force = true;
        } else if (arg == "--fpga") {
            if (i + 1 >= args.size()) {
                return UsageError{"--fpga needs CL025 or CL016"};
            }
            o.fpga = parse_fpga(args[++i]);
            if (!o.fpga) {
                return UsageError{"--fpga must be CL025 or CL016, not " + args[i]};
            }
        } else if (arg.starts_with("-")) {
            return UsageError{"unknown option " + arg};
        } else if (image_given) {
            return UsageError{"only one IMAGE"};
        } else {
            o.image = arg;
            image_given = true;
        }
    }
    return o;
}

std::optional<std::string> refusal(const Options& options, const std::optional<KnownImage>& known) {
    if (known && options.fpga && known->fpga != *options.fpga) {
        return "the image is for a " + std::string(to_string(known->fpga)) + ", not a " +
               std::string(to_string(*options.fpga));
    }
    if (options.force) {
        return std::nullopt;
    }
    if (!known) {
        return "the image is not one rbprobe knows (--force loads it anyway)";
    }
    if (known->family != GatewareFamily::kRpi5Pio) {
        return "the image is " + std::string(to_string(known->family)) +
               " gateware, which this project's driver cannot use (--force loads it anyway)";
    }
    return std::nullopt;
}

int run_app(std::span<const std::string> args, Environment& env) {
    const ParseResult parsed = parse_options(args);
    if (std::holds_alternative<ShowHelp>(parsed)) {
        env.out << kUsage;
        return kExitOk;
    }
    if (std::holds_alternative<ShowVersion>(parsed)) {
        env.out << "rbload " << kVersion << '\n';
        return kExitOk;
    }
    if (const auto* error = std::get_if<UsageError>(&parsed)) {
        env.err << "rbload: " << error->message << '\n' << kUsage;
        return kExitUsage;
    }
    const Options& options = std::get<Options>(parsed);

    std::string content;
    try {
        content = env.read_file(options.image);
    } catch (const std::exception& e) {
        env.err << "rbload: " << e.what() << '\n';
        return kExitFailed;
    }
    const std::string sha256 = Sha256::hex(content);
    const std::optional<KnownImage> known = find_known_image(sha256);
    if (const auto why = refusal(options, known)) {
        env.err << "rbload: refusing " << options.image << ": " << *why << '\n';
        return kExitUsage;
    }
    if (env.driver_loaded()) {
        env.err << "rbload: the radioberry driver is loaded and owns the configuration lines; "
                   "stop radioberry.service and rmmod radioberry first\n";
        return kExitUsage;
    }

    Node report = Node::map();
    report.set("image", image_node(options, content.size(), sha256, known));
    ConfigResult result;
    double seconds = 0;
    try {
        const std::unique_ptr<ConfigPins> pins = env.open_pins();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(content.data());
        const auto start = std::chrono::steady_clock::now();
        result = configure_passive_serial(*pins, std::span(bytes, content.size()));
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    } catch (const std::exception& e) {
        env.err << "rbload: " << e.what() << '\n';
        return kExitFailed;
    }
    Node load = Node::map();
    load.set("configured", result.ok());
    load.set("result", result.message());
    load.set("seconds", std::round(seconds * 100) / 100);
    report.set("load", load);
    render(options, report, env.out);
    return result.ok() ? kExitOk : kExitFailed;
}

}  // namespace radioberry::load
