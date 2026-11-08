// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <sstream>

#include "collect.hpp"
#include "radioberry/render.hpp"
#include "radioberry/version.hpp"

namespace radioberry::probe {
namespace {

constexpr const char* kUsage =
    "usage: rbprobe [--json | --yaml] [--identify] [--gateware-dir DIR]...\n"
    "\n"
    "Reports the state of a Raspberry Pi 5 with a Radioberry: host, boot\n"
    "configuration, driver, gateware, FPGA, PA board and software. Exits 0\n"
    "when the radio can run, 1 when it cannot, 2 on a usage error.\n"
    "\n"
    "  --json              JSON output\n"
    "  --yaml              YAML output\n"
    "  --identify          identify the FPGA by loading each known rpi-5\n"
    "                      gateware image until one configures it; changes\n"
    "                      FPGA state and needs the driver unloaded\n"
    "  --gateware-dir DIR  where --identify looks for images (repeatable;\n"
    "                      replaces the default directories)\n"
    "  --version           print the version\n"
    "  --help              this text\n";

enum class Format { kText, kJson, kYaml };

}  // namespace

std::string render_probe_text(const Node& root) {
    std::ostringstream out;
    out << render_text(root.without({"findings", "radio_can_run"}));
    if (const Node* findings = root.find("findings")) {
        out << "\nfindings\n";
        if (findings->items().empty()) {
            out << "  none\n";
        }
        for (const auto& f : findings->items()) {
            const Node* severity = f.find("severity");
            const Node* id = f.find("id");
            const Node* message = f.find("message");
            out << "  " << (severity ? scalar_text(*severity) : "") << ' '
                << (id ? scalar_text(*id) : "") << ": " << (message ? scalar_text(*message) : "")
                << '\n';
        }
    }
    if (const Node* can_run = root.find("radio_can_run")) {
        out << "\nradio can run: " << scalar_text(*can_run) << '\n';
    }
    return out.str();
}

int run_app(std::span<const std::string> args, Source& source, std::ostream& out,
            std::ostream& err) {
    Format format = Format::kText;
    Options options;
    bool custom_dirs = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--json") {
            format = Format::kJson;
        } else if (arg == "--yaml") {
            format = Format::kYaml;
        } else if (arg == "--identify") {
            options.identify = true;
        } else if (arg == "--gateware-dir") {
            if (i + 1 >= args.size()) {
                err << "rbprobe: --gateware-dir needs a directory\n" << kUsage;
                return kExitUsage;
            }
            if (!custom_dirs) {
                options.gateware_dirs.clear();
                custom_dirs = true;
            }
            options.gateware_dirs.push_back(args[++i]);
        } else if (arg == "--version") {
            out << "rbprobe " << kVersion << '\n';
            return kExitRadioCanRun;
        } else if (arg == "--help" || arg == "-h") {
            out << kUsage;
            return kExitRadioCanRun;
        } else {
            err << "rbprobe: unknown option " << arg << '\n' << kUsage;
            return kExitUsage;
        }
    }

    const Report report = collect(source, options);
    switch (format) {
        case Format::kText:
            out << render_probe_text(report.root);
            break;
        case Format::kJson:
            out << render_json(report.root);
            break;
        case Format::kYaml:
            out << render_yaml(report.root);
            break;
    }
    return report.radio_can_run() ? kExitRadioCanRun : kExitRadioCannotRun;
}

}  // namespace radioberry::probe
