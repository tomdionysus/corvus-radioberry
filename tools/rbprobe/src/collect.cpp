// SPDX-License-Identifier: GPL-2.0-or-later
#include "collect.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

#include "radioberry/gateware_catalog.hpp"
#include "radioberry/sha256.hpp"
#include "text.hpp"

namespace radioberry::probe {
namespace {

constexpr const char* kFirmwarePath = "/lib/firmware/radioberry.rbf";
constexpr const char* kDevicePath = "/dev/radioberry";
constexpr const char* kServiceName = "radioberry.service";
constexpr int kPaI2cBus = 1;

std::string hex_bytes(const std::string& bytes) {
    std::string out;
    for (const char c : bytes) {
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02x", static_cast<unsigned char>(c));
        out += buf;
    }
    return out;
}

std::string hex_address(std::uint8_t address) {
    char buf[5];
    std::snprintf(buf, sizeof buf, "0x%02x", address);
    return buf;
}

class Collector {
public:
    Collector(Source& source, const Options& options) : src_(source), opt_(options) {}

    Report run() {
        kernel_ = trim(src_.read_file("/proc/sys/kernel/osrelease").value_or(""));
        Report report;
        report.root = Node::map();
        report.root.set("host", host());
        report.root.set("boot", boot());
        report.root.set("driver", driver());
        report.root.set("firmware", firmware());
        report.root.set("fpga", fpga());
        report.root.set("pa", pa());
        report.root.set("software", software());

        Node findings = Node::seq();
        for (const auto& f : findings_) {
            Node n = Node::map();
            n.set("severity", std::string(to_string(f.severity)));
            n.set("id", f.id);
            n.set("message", f.message);
            findings.push(std::move(n));
        }
        report.root.set("findings", std::move(findings));
        report.findings = findings_;
        report.root.set("radio_can_run", report.radio_can_run());
        return report;
    }

private:
    void add(Severity severity, std::string id, std::string message) {
        findings_.push_back({severity, std::move(id), std::move(message)});
    }

    std::optional<KnownImage> lookup(const std::string& hash) const {
        for (const auto& image : opt_.catalog) {
            if (image.sha256 == hash) {
                return image;
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> file_trimmed(const std::string& path) {
        auto content = src_.read_file(path);
        if (!content) {
            return std::nullopt;
        }
        return trim(*content);
    }

    // ---- host ---------------------------------------------------------

    Node host() {
        Node n = Node::map();
        n.set("model", file_trimmed("/proc/device-tree/model"));

        Node revision;
        if (auto raw = src_.read_file("/proc/device-tree/system/linux,revision")) {
            std::string hex = hex_bytes(*raw);
            hex.erase(0, std::min(hex.find_first_not_of('0'), hex.size()));
            revision = hex;
        }
        n.set("revision", revision);
        n.set("serial", file_trimmed("/proc/device-tree/serial-number"));

        Node os;
        if (auto release = src_.read_file("/etc/os-release")) {
            os = unquote(key_value(*release, "PRETTY_NAME"));
        }
        n.set("os", os);
        n.set("kernel", kernel_.empty() ? Node() : Node(kernel_));

        const std::string build = "/lib/modules/" + kernel_ + "/build";
        const bool headers = !kernel_.empty() && src_.exists(build);
        Node h = Node::map();
        h.set("present", headers);
        h.set("path", build);
        n.set("headers", std::move(h));
        if (!headers) {
            add(Severity::kWarning, "host.headers",
                "kernel headers for " + kernel_ + " are not installed; the driver cannot be built");
        }

        n.set("bootloader", bootloader());
        n.set("pmic", pmic());
        n.set("throttled", throttled());
        n.set("network", network());
        return n;
    }

    Node bootloader() {
        Node n = Node::map();
        auto r = src_.run({"vcgencmd", "bootloader_version"});
        if (!r || r->exit_status != 0) {
            n.set("date", Node());
            n.set("version", Node());
            return n;
        }
        const auto lines = split_lines(r->output);
        n.set("date", lines.empty() ? Node() : Node(trim(lines[0])));
        Node version;
        for (const auto& line : lines) {
            const auto words = split_words(line);
            if (words.size() >= 2 && words[0] == "version") {
                version = words[1];
            }
        }
        n.set("version", version);
        return n;
    }

    Node pmic() {
        Node n = Node::map();
        auto r = src_.run({"vcgencmd", "pmic_read_adc"});
        if (!r || r->exit_status != 0) {
            return Node();
        }
        // Lines look like "  3V3_SYS_A current(1)=0.06831510A".
        for (const auto& line : split_lines(r->output)) {
            const auto words = split_words(line);
            if (words.size() != 2) {
                continue;
            }
            const auto eq = words[1].find('=');
            if (eq == std::string::npos) {
                continue;
            }
            n.set(words[0], words[1].substr(eq + 1));
        }
        return n;
    }

    Node throttled() {
        Node n = Node::map();
        auto r = src_.run({"vcgencmd", "get_throttled"});
        if (!r || r->exit_status != 0) {
            return Node();
        }
        const std::string value = trim(r->output);
        const auto eq = value.find('=');
        unsigned long bits = 0;
        try {
            bits = std::stoul(value.substr(eq == std::string::npos ? 0 : eq + 1), nullptr, 16);
        } catch (const std::exception&) {
            return Node();
        }
        static const std::map<unsigned, const char*> kFlags = {
            {0, "under-voltage"},
            {1, "arm-frequency-capped"},
            {2, "throttled"},
            {3, "soft-temperature-limit"},
            {16, "under-voltage-occurred"},
            {17, "arm-frequency-capping-occurred"},
            {18, "throttling-occurred"},
            {19, "soft-temperature-limit-occurred"},
        };
        char raw[16];
        std::snprintf(raw, sizeof raw, "0x%lx", bits);
        n.set("raw", raw);
        Node flags = Node::seq();
        for (const auto& [bit, name] : kFlags) {
            if ((bits >> bit) & 1UL) {
                flags.push(name);
            }
        }
        n.set("flags", std::move(flags));
        if (bits & 0xfUL) {
            add(Severity::kWarning, "host.throttled",
                "the Pi is throttled or under-voltage now (" + std::string(raw) + ")");
        } else if (bits & 0xf0000UL) {
            add(Severity::kInfo, "host.throttled",
                "throttling or under-voltage occurred since boot (" + std::string(raw) + ")");
        }
        return n;
    }

    Node network() {
        Node list = Node::seq();
        auto names = src_.list_dir("/sys/class/net");
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            const std::string base = "/sys/class/net/" + name;
            if (!src_.exists(base + "/device")) {
                continue;  // loopback, bridges and other virtual links
            }
            Node n = Node::map();
            n.set("name", name);
            n.set("kind", src_.exists(base + "/wireless") ? "wifi" : "ethernet");
            n.set("state", file_trimmed(base + "/operstate"));
            Node carrier;
            if (auto c = file_trimmed(base + "/carrier")) {
                carrier = (*c == "1");
            }
            n.set("carrier", carrier);
            list.push(std::move(n));
        }
        return list;
    }

    // ---- boot ---------------------------------------------------------

    Node boot() {
        Node n = Node::map();

        Node overlay = Node::map();
        const auto compatible = src_.read_file("/proc/device-tree/radioberry/compatible");
        const bool node_ok = compatible && compatible->find("sdr,radioberry") != std::string::npos;
        overlay.set("node", node_ok);
        overlay.set("status", file_trimmed("/proc/device-tree/radioberry/status"));
        n.set("overlay", std::move(overlay));
        if (!node_ok) {
            add(Severity::kBlocker, "boot.overlay",
                "no sdr,radioberry device tree node; add dtoverlay=radioberry to config.txt");
        }

        Node spi = Node::map();
        const std::string spi_dir = "/sys/bus/spi/devices/spi0.0";
        const bool spi_present = src_.exists(spi_dir);
        spi.set("present", spi_present);
        const auto modalias = file_trimmed(spi_dir + "/modalias");
        spi.set("modalias", modalias);
        Node bound;
        if (auto link = src_.read_link(spi_dir + "/driver")) {
            bound = base_name(*link);
        }
        spi.set("driver", bound);
        n.set("spi0_0", std::move(spi));
        if (!spi_present || modalias.value_or("") != "spi:radioberry_ctrl_spi") {
            add(Severity::kBlocker, "boot.spi",
                "spi0.0 is not the radioberry_ctrl_spi device created by the overlay");
        }

        const bool pio = src_.exists("/sys/module/rp1_pio");
        n.set("rp1_pio_loaded", pio);
        if (!pio) {
            add(Severity::kBlocker, "boot.rp1_pio", "the rp1_pio kernel module is not loaded");
        }

        const bool i2c = src_.exists("/dev/i2c-1");
        n.set("i2c_arm", i2c);
        if (!i2c) {
            add(Severity::kWarning, "boot.i2c",
                "I2C bus 1 is off (dtparam=i2c_arm=on); PA and filter boards cannot be probed");
        }

        Node spidev = Node::seq();
        auto devs = src_.list_dir("/dev");
        std::sort(devs.begin(), devs.end());
        for (const auto& d : devs) {
            if (d.rfind("spidev", 0) == 0) {
                spidev.push(d);
                if (d == "spidev0.0") {
                    add(Severity::kBlocker, "boot.spidev",
                        "spidev0.0 exists and conflicts with the Radioberry control SPI");
                }
            }
        }
        n.set("spidev", std::move(spidev));
        return n;
    }

    // ---- driver -------------------------------------------------------

    Node driver() {
        Node n = Node::map();
        Node module_file;
        if (!kernel_.empty()) {
            const std::string root = "/lib/modules/" + kernel_ + "/";
            if (auto dep = src_.read_file(root + "modules.dep")) {
                for (const auto& line : split_lines(*dep)) {
                    const std::string path = line.substr(0, line.find(':'));
                    if (base_name(path).rfind("radioberry.ko", 0) == 0) {
                        module_file = root + path;
                        break;
                    }
                }
            }
        }
        const bool have_module = !module_file.is_null();
        n.set("module_file", module_file);
        loaded_ = src_.exists("/sys/module/radioberry");
        n.set("loaded", loaded_);
        const bool device = src_.exists(kDevicePath);
        n.set("device", device);

        Node holders = Node::seq();
        if (device) {
            for (const auto& pid : src_.list_dir("/proc")) {
                if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](unsigned char c) {
                        return std::isdigit(c) != 0;
                    })) {
                    continue;
                }
                const std::string fd_dir = "/proc/" + pid + "/fd";
                for (const auto& fd : src_.list_dir(fd_dir)) {
                    if (src_.read_link(fd_dir + "/" + fd).value_or("") == kDevicePath) {
                        Node h = Node::map();
                        h.set("pid", std::stol(pid));
                        h.set("command", file_trimmed("/proc/" + pid + "/comm"));
                        holders.push(std::move(h));
                        break;
                    }
                }
            }
        }
        const bool held = !holders.items().empty();
        n.set("holders", std::move(holders));

        if (!have_module) {
            add(Severity::kBlocker, "driver.module",
                "no radioberry kernel module is installed for kernel " + kernel_);
        } else if (!loaded_) {
            add(Severity::kBlocker, "driver.loaded", "the radioberry module is not loaded");
        }
        if (loaded_ && !device) {
            add(Severity::kBlocker, "driver.device",
                "the module is loaded but /dev/radioberry does not exist");
        }
        if (held) {
            add(Severity::kInfo, "driver.busy", "/dev/radioberry is open by another process");
        }
        return n;
    }

    // ---- firmware -----------------------------------------------------

    Node firmware() {
        Node n = Node::map();
        n.set("path", kFirmwarePath);
        const auto content = src_.read_file(kFirmwarePath);
        n.set("present", content.has_value());
        if (!content) {
            add(Severity::kBlocker, "firmware.missing",
                std::string(kFirmwarePath) + " is missing; the driver has no gateware to load");
            return n;
        }
        n.set("size", static_cast<unsigned long>(content->size()));
        const std::string hash = Sha256::hex(*content);
        n.set("sha256", hash);
        installed_ = lookup(hash);
        n.set("known", installed_.has_value());
        if (!installed_) {
            n.set("family", Node());
            n.set("fpga", Node());
            n.set("upstream", Node());
            add(Severity::kWarning, "firmware.unknown",
                "the installed gateware image is not a known upstream image");
            return n;
        }
        n.set("family", std::string(to_string(installed_->family)));
        n.set("fpga", std::string(to_string(installed_->fpga)));
        n.set("upstream",
              std::string(installed_->upstream_ref) + " " + std::string(installed_->upstream_path));
        if (installed_->family != GatewareFamily::kRpi5Pio) {
            add(Severity::kBlocker, "firmware.family",
                "the installed gateware is the " + std::string(to_string(installed_->family)) +
                    " image; a Raspberry Pi 5 needs the rpi-5 PIO image");
        }
        return n;
    }

    // ---- fpga ---------------------------------------------------------

    Node config_lines() {
        Node n = Node::map();
        const auto lines = src_.read_config_status();
        n.set("available", lines.available);
        if (lines.available) {
            n.set("conf_done", lines.conf_done);
            n.set("nstatus", lines.nstatus);
            n.set("configured", lines.conf_done && lines.nstatus);
        } else {
            n.set("consumer", lines.consumer);
        }
        return n;
    }

    std::vector<std::pair<std::string, std::string>> candidate_files() {
        std::vector<std::pair<std::string, std::string>> files;  // path, contents
        std::set<std::string> seen;
        auto consider = [&](const std::string& path) {
            if (path.size() < 4 || path.substr(path.size() - 4) != ".rbf") {
                return;
            }
            auto content = src_.read_file(path);
            if (!content) {
                return;
            }
            const std::string hash = Sha256::hex(*content);
            if (seen.insert(hash).second) {
                files.emplace_back(path, std::move(*content));
            }
        };
        for (const auto& dir : opt_.gateware_dirs) {
            auto names = src_.list_dir(dir);
            std::sort(names.begin(), names.end());
            for (const auto& name : names) {
                const std::string path = dir + "/" + name;
                consider(path);
                auto inner = src_.list_dir(path);
                std::sort(inner.begin(), inner.end());
                for (const auto& sub : inner) {
                    consider(path + "/" + sub);
                }
            }
        }
        return files;
    }

    Node identify() {
        Node n = Node::map();
        if (loaded_) {
            n.set("attempted", false);
            n.set("reason", "the radioberry driver is loaded and owns the configuration lines");
            add(Severity::kWarning, "fpga.identify",
                "identification skipped: unload the radioberry module first");
            return n;
        }
        n.set("attempted", true);

        struct Candidate {
            std::string path;
            std::string content;
            KnownImage image;
        };
        std::vector<Candidate> candidates;
        for (auto& [path, content] : candidate_files()) {
            auto known = lookup(Sha256::hex(content));
            if (known && known->family == GatewareFamily::kRpi5Pio) {
                candidates.push_back({path, std::move(content), *known});
            }
        }
        // Catalog order: the most likely FPGA first.
        const auto catalog = opt_.catalog;
        auto rank = [&](const Candidate& c) {
            for (std::size_t i = 0; i < catalog.size(); ++i) {
                if (catalog[i].sha256 == c.image.sha256) {
                    return i;
                }
            }
            return catalog.size();
        };
        std::sort(candidates.begin(), candidates.end(),
                  [&](const Candidate& a, const Candidate& b) { return rank(a) < rank(b); });

        Node attempts = Node::seq();
        for (const auto& c : candidates) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(c.content.data());
            const ConfigResult result =
                src_.configure_fpga(std::span<const std::uint8_t>(bytes, c.content.size()));
            Node a = Node::map();
            a.set("image", c.path);
            a.set("fpga", std::string(to_string(c.image.fpga)));
            a.set("result", result.message());
            attempts.push(std::move(a));
            if (result.ok()) {
                identified_ = c.image.fpga;
                break;
            }
        }
        n.set("attempts", std::move(attempts));
        if (candidates.empty()) {
            add(Severity::kWarning, "fpga.identify",
                "identification found no known rpi-5 image in the gateware directories");
        } else if (!identified_) {
            add(Severity::kBlocker, "fpga.identify",
                "no known rpi-5 image configured the FPGA; check the board and its power");
        }
        return n;
    }

    Node fpga() {
        Node n = Node::map();
        if (opt_.identify) {
            n.set("identify", identify());
        }
        n.set("config_lines", config_lines());

        Node type;
        Node type_source = "unknown";
        if (identified_) {
            type = std::string(to_string(*identified_));
            type_source = "identify";
        }
        n.set("type", type);
        n.set("type_source", type_source);
        // Gateware identity is read through the driver's ioctl interface,
        // which does not exist yet; it is reported as absent.
        n.set("gateware", Node());

        if (!identified_) {
            add(Severity::kInfo, "fpga.type",
                loaded_ ? "FPGA type not known: the driver does not report it yet"
                        : "FPGA type not known: run rbprobe --identify with the driver unloaded");
        }
        if (identified_ && installed_ && installed_->fpga != *identified_) {
            add(Severity::kBlocker, "firmware.fpga",
                "the installed gateware is for a " + std::string(to_string(installed_->fpga)) +
                    " but the FPGA is a " + std::string(to_string(*identified_)));
        }
        return n;
    }

    // ---- pa -----------------------------------------------------------

    Node pa() {
        Node n = Node::map();
        n.set("i2c_bus", kPaI2cBus);
        const auto devices = src_.scan_i2c(kPaI2cBus);
        if (!devices) {
            n.set("devices", Node());
            n.set("board", Node());
            return n;
        }
        static const std::map<std::uint8_t, const char*> kKnown = {
            {0x20, "N2ADR filter board (MCP23008)"},
            {0x21, "Alex filter interface (PCA9555)"},
            {0x22, "VA2SAJ generic filter board"},
            {0x2c, "MCP4662 PA bias potentiometer"},
            {0x34, "MAX11613 PA current and temperature ADC"},
        };
        Node list = Node::seq();
        bool adc = false;
        bool bias = false;
        bool filters = false;
        for (const auto& d : *devices) {
            Node dn = Node::map();
            dn.set("address", hex_address(d.address));
            const auto it = kKnown.find(d.address);
            dn.set("device", it == kKnown.end() ? "unknown" : it->second);
            dn.set("claimed_by_driver", d.claimed_by_driver);
            list.push(std::move(dn));
            adc = adc || d.address == 0x34;
            bias = bias || d.address == 0x2c;
            filters = filters || (d.address >= 0x20 && d.address <= 0x22);
        }
        n.set("devices", std::move(list));
        n.set("telemetry", adc);
        n.set("digital_bias", bias);
        n.set("filter_board", filters);

        std::string board;
        if (adc && bias) {
            board = "pa3gsb 5 W preamp: digital bias and telemetry";
        } else if (adc) {
            board = "PA with current and temperature telemetry, manual bias";
        } else if (bias) {
            board = "PA with digital bias, no telemetry";
        } else {
            board = "manual-bias board, no telemetry";
        }
        n.set("board", board);
        if (!adc) {
            add(Severity::kInfo, "pa.telemetry",
                "no PA telemetry: PA current and temperature cannot be read, no interlock");
        }
        return n;
    }

    // ---- software -----------------------------------------------------

    Node software() {
        Node n = Node::map();

        Node daemon = Node::map();
        Node path;
        Node version;
        for (const char* candidate : {"/usr/local/bin/radioberryd", "/usr/bin/radioberryd"}) {
            if (src_.exists(candidate)) {
                path = candidate;
                if (auto r = src_.run({candidate, "--version"}); r && r->exit_status == 0) {
                    version = trim(r->output);
                }
                break;
            }
        }
        daemon.set("path", path);
        daemon.set("version", version);
        daemon.set("service_enabled", systemctl("is-enabled"));
        daemon.set("service_active", systemctl("is-active"));
        n.set("daemon", std::move(daemon));
        if (path.is_null()) {
            add(Severity::kBlocker, "software.daemon", "radioberryd is not installed");
        }
        if (src_.exists("/etc/init.d/radioberryd")) {
            add(Severity::kWarning, "software.legacy",
                "the upstream init script /etc/init.d/radioberryd is installed");
        }

        n.set("udp_ports", udp_ports());
        n.set("liquid_dsp", dpkg_version("libliquid"));

        Node pihpsdr = Node::map();
        Node pihpsdr_path;
        for (const char* candidate : {"/usr/local/bin/pihpsdr", "/usr/bin/pihpsdr"}) {
            if (src_.exists(candidate)) {
                pihpsdr_path = candidate;
                break;
            }
        }
        pihpsdr.set("path", pihpsdr_path);
        // piHPSDR is a GUI program without a version option; it is not run.
        pihpsdr.set("version", dpkg_version("pihpsdr"));
        n.set("pihpsdr", std::move(pihpsdr));
        return n;
    }

    Node systemctl(const char* verb) {
        auto r = src_.run({"systemctl", verb, kServiceName});
        if (!r) {
            return Node();
        }
        const std::string state = trim(r->output);
        return state.empty() ? Node() : Node(state);
    }

    Node udp_ports() {
        // Protocol 1 uses 1024; Protocol 2 uses 1024 to 1029 on the radio.
        Node list = Node::seq();
        std::set<std::pair<int, std::string>> seen;
        for (const char* table : {"udp", "udp6"}) {
            auto content = src_.read_file(std::string("/proc/net/") + table);
            if (!content) {
                continue;
            }
            const auto lines = split_lines(*content);
            for (std::size_t i = 1; i < lines.size(); ++i) {
                const auto words = split_words(lines[i]);
                if (words.size() < 2) {
                    continue;
                }
                const auto colon = words[1].rfind(':');
                if (colon == std::string::npos) {
                    continue;
                }
                int port = 0;
                try {
                    port = std::stoi(words[1].substr(colon + 1), nullptr, 16);
                } catch (const std::exception&) {
                    continue;
                }
                if (port >= 1024 && port <= 1029 && seen.insert({port, table}).second) {
                    Node p = Node::map();
                    p.set("port", port);
                    p.set("family", std::string(table) == "udp" ? "ipv4" : "ipv6");
                    list.push(std::move(p));
                }
            }
        }
        return list;
    }

    Node dpkg_version(const std::string& prefix) {
        auto status = src_.read_file("/var/lib/dpkg/status");
        if (!status) {
            return Node();
        }
        std::string package;
        std::string state;
        std::string version;
        Node found;
        auto flush = [&] {
            if (found.is_null() && package.rfind(prefix, 0) == 0 &&
                state.find("installed") != std::string::npos &&
                state.find("not-installed") == std::string::npos && !version.empty()) {
                found = version;
            }
            package.clear();
            state.clear();
            version.clear();
        };
        for (const auto& line : split_lines(*status)) {
            if (trim(line).empty()) {
                flush();
            } else if (line.rfind("Package: ", 0) == 0) {
                package = line.substr(9);
            } else if (line.rfind("Status: ", 0) == 0) {
                state = line.substr(8);
            } else if (line.rfind("Version: ", 0) == 0) {
                version = line.substr(9);
            }
        }
        flush();
        return found;
    }

    Source& src_;
    const Options& opt_;
    std::string kernel_;
    bool loaded_ = false;
    std::optional<KnownImage> installed_;
    std::optional<FpgaType> identified_;
    std::vector<Finding> findings_;
};

}  // namespace

bool Report::radio_can_run() const {
    return std::none_of(findings.begin(), findings.end(),
                        [](const Finding& f) { return f.severity == Severity::kBlocker; });
}

std::string_view to_string(Severity severity) {
    switch (severity) {
        case Severity::kBlocker:
            return "blocker";
        case Severity::kWarning:
            return "warning";
        case Severity::kInfo:
            return "info";
    }
    return "unknown";
}

Report collect(Source& source, const Options& options) { return Collector(source, options).run(); }

}  // namespace radioberry::probe
