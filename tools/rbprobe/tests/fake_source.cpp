// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_source.hpp"

#include <array>

#include "radioberry/sha256.hpp"

namespace radioberry::probe::test {
namespace {

bool under(const std::string& path, const std::string& dir) {
    return path.size() > dir.size() + 1 && path.compare(0, dir.size(), dir) == 0 &&
           path[dir.size()] == '/';
}

std::string child_name(const std::string& path, const std::string& dir) {
    const std::string rest = path.substr(dir.size() + 1);
    return rest.substr(0, rest.find('/'));
}

}  // namespace

std::optional<std::string> FakeSource::read_file(const std::string& path) {
    const auto it = files.find(path);
    if (it == files.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool FakeSource::exists(const std::string& path) {
    if (files.contains(path) || links.contains(path) || dirs.contains(path)) {
        return true;
    }
    for (const auto* container : {&files, &links}) {
        for (const auto& [p, unused] : *container) {
            if (under(p, path)) {
                return true;
            }
        }
    }
    for (const auto& d : dirs) {
        if (under(d, path)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> FakeSource::list_dir(const std::string& path) {
    std::set<std::string> names;
    for (const auto* container : {&files, &links}) {
        for (const auto& [p, unused] : *container) {
            if (under(p, path)) {
                names.insert(child_name(p, path));
            }
        }
    }
    for (const auto& d : dirs) {
        if (under(d, path)) {
            names.insert(child_name(d, path));
        }
    }
    return {names.begin(), names.end()};
}

std::optional<std::string> FakeSource::read_link(const std::string& path) {
    const auto it = links.find(path);
    if (it == links.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<CommandResult> FakeSource::run(const std::vector<std::string>& argv) {
    commands_run.push_back(argv);
    const auto it = commands.find(argv);
    if (it == commands.end()) {
        return std::nullopt;
    }
    return it->second;
}

ConfigStatusLines FakeSource::read_config_status() { return config_lines; }

std::optional<std::vector<I2cDevice>> FakeSource::scan_i2c(int bus) {
    if (bus != 1) {
        return std::nullopt;
    }
    return i2c;
}

ConfigResult FakeSource::configure_fpga(std::span<const std::uint8_t> image) {
    ++configure_calls;
    if (configure) {
        return configure(image);
    }
    return {ConfigError::kConfDoneLow};
}

std::span<const KnownImage> test_catalog() {
    static const std::string cl025 = Sha256::hex(kCl025Image);
    static const std::string cl016 = Sha256::hex(kCl016Image);
    static const std::string rpi4 = Sha256::hex(kRpi4Image);
    static const std::array<KnownImage, 3> catalog = {{
        {cl025, GatewareFamily::kRpi5Pio, FpgaType::k10CL025, kCl025Image.size(),
         "SBC/rpi-5/releases/dev/CL025/radioberry.rbf", "test"},
        {cl016, GatewareFamily::kRpi5Pio, FpgaType::k10CL016, kCl016Image.size(),
         "SBC/rpi-5/releases/dev/CL016/radioberry.rbf", "test"},
        {rpi4, GatewareFamily::kRpi4Gpio, FpgaType::k10CL016, kRpi4Image.size(),
         "SBC/rpi-4/releases/dev/CL016/radioberry.rbf", "test"},
    }};
    return catalog;
}

FakeSource development_pi() {
    FakeSource s;
    const std::string k = "6.18.60+rpt-rpi-2712";
    s.files["/proc/sys/kernel/osrelease"] = k + "\n";
    s.files["/proc/device-tree/model"] = std::string("Raspberry Pi 5 Model B Rev 1.0") + '\0';
    s.files["/proc/device-tree/system/linux,revision"] = std::string("\x00\xd0\x41\x70", 4);
    s.files["/proc/device-tree/serial-number"] = std::string("0123456789abcdef") + '\0';
    s.files["/etc/os-release"] =
        "PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\nNAME=\"Debian GNU/Linux\"\n";
    s.dirs.insert("/lib/modules/" + k + "/build");
    s.files["/lib/modules/" + k + "/modules.dep"] =
        "kernel/drivers/misc/rp1-pio.ko.xz:\nkernel/drivers/i2c/i2c-dev.ko.xz:\n";

    s.commands[{"vcgencmd", "bootloader_version"}] = {
        0,
        "2026/10/30 20:55:54\nversion 0123456789abcdef0123456789abcdef01234567 (release)\n"
        "timestamp 1793393754\n"};
    s.commands[{"vcgencmd", "pmic_read_adc"}] = {
        0,
        " 3V7_WL_SW_A current(0)=0.05562801A\n   3V3_SYS_A current(1)=0.06831510A\n"
        "   EXT5V_V volt(24)=5.13600000V\nnonsense line here\nnoequals x\n"};
    s.commands[{"vcgencmd", "get_throttled"}] = {0, "throttled=0x0\n"};

    s.dirs.insert("/sys/class/net/lo");
    s.files["/sys/class/net/lo/operstate"] = "unknown\n";
    s.links["/sys/class/net/eth0/device"] = "../../../0000:01:00.0";
    s.files["/sys/class/net/eth0/operstate"] = "down\n";
    s.links["/sys/class/net/wlan0/device"] = "../../../mmc1:0001:1";
    s.dirs.insert("/sys/class/net/wlan0/wireless");
    s.files["/sys/class/net/wlan0/operstate"] = "up\n";
    s.files["/sys/class/net/wlan0/carrier"] = "1\n";

    s.files["/proc/device-tree/radioberry/compatible"] = std::string("sdr,radioberry") + '\0';
    s.files["/proc/device-tree/radioberry/status"] = std::string("okay") + '\0';
    s.files["/sys/bus/spi/devices/spi0.0/modalias"] = "spi:radioberry_ctrl_spi\n";
    s.dirs.insert("/sys/module/rp1_pio");
    s.files["/dev/i2c-1"] = "";
    s.files["/dev/null"] = "";

    s.files["/lib/firmware/radioberry.rbf"] = kRpi4Image;
    s.files["/srv/gateware/CL025/radioberry.rbf"] = kCl025Image;
    s.files["/srv/gateware/CL016/radioberry.rbf"] = kCl016Image;
    s.files["/srv/gateware/README"] = "int main() {}";

    s.config_lines = {true, "", true, true};
    s.i2c = std::vector<I2cDevice>{};
    s.configure = [](std::span<const std::uint8_t> image) {
        const std::string content(reinterpret_cast<const char*>(image.data()), image.size());
        return content == kCl025Image ? ConfigResult{} : ConfigResult{ConfigError::kConfDoneLow};
    };

    s.files["/etc/systemd/system/radioberry.service"] = "[Service]\n";
    s.files["/etc/init.d/radioberryd"] = "#!/bin/sh\n";
    s.commands[{"systemctl", "is-enabled", "radioberry.service"}] = {1, "disabled\n"};
    s.commands[{"systemctl", "is-active", "radioberry.service"}] = {3, "inactive\n"};
    s.files["/proc/net/udp"] =
        "  sl  local_address rem_address   st\n"
        " 2424: 00000000:BEAB 00000000:0000 07 00000000:00000000\n"
        " 2833: E3012C0A:0044 01012C0A:0043 01 00000000:00000000\n";
    s.files["/var/lib/dpkg/status"] =
        "Package: libliquid-dev\nStatus: install ok installed\nVersion: 1.7.0-1\n\n"
        "Package: libliquid1\nStatus: install ok installed\nVersion: 1.7.0-1\n";
    s.files["/usr/local/bin/pihpsdr"] = "";
    return s;
}

FakeSource working_pi() {
    FakeSource s = development_pi();
    const std::string k = "6.18.60+rpt-rpi-2712";
    s.files["/lib/modules/" + k + "/modules.dep"] =
        "kernel/drivers/misc/rp1-pio.ko.xz:\nupdates/radioberry.ko: kernel/drivers/misc/"
        "rp1-pio.ko.xz\n";
    s.dirs.insert("/sys/module/radioberry");
    s.files["/dev/radioberry"] = "";
    s.links["/sys/bus/spi/devices/spi0.0/driver"] = "../../../../bus/spi/drivers/radioberry";
    s.files["/lib/firmware/radioberry.rbf"] = kCl025Image;
    s.files.erase("/proc/device-tree/radioberry/compatible");
    s.files.erase("/proc/device-tree/radioberry/status");
    s.files["/sys/bus/spi/devices/spi0.0/modalias"] = "spi:radioberry\n";
    s.files["/sys/bus/spi/devices/spi0.0/of_node/compatible"] =
        std::string("sdr,radioberry") + '\0';
    s.files["/sys/bus/spi/devices/spi0.0/gateware_version"] = "75.3\n";
    s.files["/sys/bus/spi/devices/spi0.0/fpga_type"] = "10CL025\n";
    s.files["/sys/bus/spi/devices/spi0.0/receivers"] = "6\n";
    s.files["/sys/bus/spi/devices/spi0.0/transmitters"] = "1\n";
    s.files.erase("/etc/init.d/radioberryd");
    s.files["/usr/local/sbin/radioberryd"] = "";
    s.commands[{"/usr/local/sbin/radioberryd", "--version"}] = {0, "radioberryd v0.1.0\n"};
    s.commands[{"systemctl", "is-enabled", "radioberry.service"}] = {0, "enabled\n"};
    s.commands[{"systemctl", "is-active", "radioberry.service"}] = {0, "active\n"};
    s.config_lines = {false, "radioberry", false, false};
    s.dirs.insert("/proc/1");
    s.links["/proc/812/fd/0"] = "/dev/null";
    s.links["/proc/812/fd/7"] = "/dev/radioberry";
    s.files["/proc/812/comm"] = "radioberryd\n";
    s.dirs.insert("/proc/self");
    s.files["/proc/net/udp6"] =
        "  sl  local_address rem_address   st\n"
        "   1: 00000000000000000000000000000000:0400 00000000000000000000000000000000:0000 07\n"
        "   2: bad\n   3: 0000:zz 0000:0000 07\n";
    s.files["/proc/net/udp"] =
        "  sl  local_address rem_address   st\n"
        " 1: 00000000:0400 00000000:0000 07\n"
        " 2: 00000000:040B 00000000:0000 07\n"
        " 3: 00000000:0429 00000000:0000 07\n";
    return s;
}

}  // namespace radioberry::probe::test
