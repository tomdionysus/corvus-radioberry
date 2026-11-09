// SPDX-License-Identifier: GPL-2.0-or-later
#include "collect.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "fake_source.hpp"
#include "radioberry/sha256.hpp"

using namespace radioberry;
using namespace radioberry::probe;
using namespace radioberry::probe::test;

namespace {

Options test_options(bool identify = false) {
    Options o;
    o.identify = identify;
    o.gateware_dirs = {"/srv/gateware", "/lib/firmware"};
    o.catalog = test_catalog();
    return o;
}

bool has(const Report& r, const std::string& id, Severity severity) {
    return std::any_of(r.findings.begin(), r.findings.end(),
                       [&](const Finding& f) { return f.id == id && f.severity == severity; });
}

bool has_id(const Report& r, const std::string& id) {
    return std::any_of(r.findings.begin(), r.findings.end(),
                       [&](const Finding& f) { return f.id == id; });
}

std::string str(const Report& r, const std::string& path) {
    const Node* n = r.root.at(path);
    REQUIRE(n != nullptr);
    REQUIRE(n->kind() == Node::Kind::kString);
    return n->as_string();
}

bool flag(const Report& r, const std::string& path) {
    const Node* n = r.root.at(path);
    REQUIRE(n != nullptr);
    REQUIRE(n->kind() == Node::Kind::kBool);
    return n->as_bool();
}

}  // namespace

TEST_CASE("development Pi: acceptance facts", "[collect]") {
    FakeSource pi = development_pi();
    const Report r = collect(pi, test_options(true));

    // 10CL025 identified, the CL016 image never tried after success.
    CHECK(str(r, "fpga.type") == "10CL025");
    CHECK(str(r, "fpga.type_source") == "identify");
    CHECK(pi.configure_calls == 1);
    const Node* attempts = r.root.at("fpga.identify.attempts");
    REQUIRE(attempts != nullptr);
    REQUIRE(attempts->items().size() == 1);
    CHECK(attempts->items()[0].find("image")->as_string() == "/srv/gateware/CL025/radioberry.rbf");

    // Wrong installed image: rpi-4 family and CL016.
    CHECK(str(r, "firmware.family") == "rpi-4 GPIO");
    CHECK(str(r, "firmware.fpga") == "10CL016");
    CHECK(has(r, "firmware.family", Severity::kBlocker));
    CHECK(has(r, "firmware.fpga", Severity::kBlocker));

    // No I2C devices.
    CHECK(r.root.at("pa.devices")->items().empty());
    CHECK(str(r, "pa.board") == "manual-bias board, no telemetry");
    CHECK(has(r, "pa.telemetry", Severity::kInfo));

    // Driver absent.
    CHECK(r.root.at("driver.module_file")->is_null());
    CHECK_FALSE(flag(r, "driver.loaded"));
    CHECK(has(r, "driver.module", Severity::kBlocker));

    CHECK_FALSE(r.radio_can_run());
    CHECK_FALSE(flag(r, "radio_can_run"));
}

TEST_CASE("development Pi: host section", "[collect]") {
    FakeSource pi = development_pi();
    const Report r = collect(pi, test_options());
    CHECK(str(r, "host.model") == "Raspberry Pi 5 Model B Rev 1.0");
    CHECK(str(r, "host.revision") == "d04170");
    CHECK(str(r, "host.serial") == "0123456789abcdef");
    CHECK(str(r, "host.os") == "Debian GNU/Linux 13 (trixie)");
    CHECK(str(r, "host.kernel") == "6.18.60+rpt-rpi-2712");
    CHECK(flag(r, "host.headers.present"));
    CHECK(str(r, "host.bootloader.date") == "2026/10/30 20:55:54");
    CHECK(str(r, "host.bootloader.version") == "0123456789abcdef0123456789abcdef01234567");
    CHECK(str(r, "host.pmic.3V3_SYS_A") == "0.06831510A");
    CHECK(str(r, "host.pmic.EXT5V_V") == "5.13600000V");
    CHECK(r.root.at("host.pmic")->entries().size() == 3);
    CHECK(str(r, "host.throttled.raw") == "0x0");
    CHECK(r.root.at("host.throttled.flags")->items().empty());

    const auto& nets = r.root.at("host.network")->items();
    REQUIRE(nets.size() == 2);
    CHECK(nets[0].find("name")->as_string() == "eth0");
    CHECK(nets[0].find("kind")->as_string() == "ethernet");
    CHECK(nets[0].find("carrier")->is_null());
    CHECK(nets[1].find("kind")->as_string() == "wifi");
    CHECK(nets[1].find("carrier")->as_bool());
}

TEST_CASE("development Pi: boot, firmware and software", "[collect]") {
    FakeSource pi = development_pi();
    const Report r = collect(pi, test_options());
    CHECK(flag(r, "boot.overlay.original"));
    CHECK_FALSE(flag(r, "boot.overlay.active"));
    CHECK(has(r, "boot.overlay", Severity::kBlocker));
    CHECK(str(r, "boot.spi0_0.modalias") == "spi:radioberry_ctrl_spi");
    CHECK(r.root.at("boot.spi0_0.driver")->is_null());
    CHECK(flag(r, "boot.rp1_pio_loaded"));
    CHECK(flag(r, "boot.i2c_arm"));
    CHECK(r.root.at("boot.spidev")->items().empty());

    CHECK(flag(r, "firmware.known"));
    CHECK(r.root.at("firmware.size")->as_int() == static_cast<long>(kRpi4Image.size()));
    CHECK(str(r, "firmware.source").find("rpi-4") != std::string::npos);

    CHECK(r.root.at("software.daemon.path")->is_null());
    CHECK(str(r, "software.daemon.service_enabled") == "disabled");
    CHECK(str(r, "software.daemon.service_active") == "inactive");
    CHECK(has(r, "software.daemon", Severity::kBlocker));
    CHECK(has(r, "software.legacy", Severity::kWarning));
    CHECK(r.root.at("software.udp_ports")->items().empty());
    CHECK(str(r, "software.liquid_dsp") == "1.7.0-1");
    CHECK(str(r, "software.pihpsdr.path") == "/usr/local/bin/pihpsdr");
    CHECK(r.root.at("software.pihpsdr.version")->is_null());

    // Without --identify the FPGA is not touched and its type is unknown.
    CHECK(pi.configure_calls == 0);
    CHECK(r.root.at("fpga.identify") == nullptr);
    CHECK(r.root.at("fpga.type")->is_null());
    CHECK(str(r, "fpga.type_source") == "unknown");
    CHECK(flag(r, "fpga.config_lines.configured"));
    CHECK(has(r, "fpga.type", Severity::kInfo));
    CHECK_FALSE(has_id(r, "firmware.fpga"));
}

TEST_CASE("working Pi: radio can run", "[collect]") {
    FakeSource pi = working_pi();
    const Report r = collect(pi, test_options());
    for (const auto& f : r.findings) {
        INFO(f.id << ": " << f.message);
        CHECK(f.severity != Severity::kBlocker);
    }
    CHECK(r.radio_can_run());
    CHECK(str(r, "driver.module_file") ==
          "/lib/modules/6.18.60+rpt-rpi-2712/updates/radioberry.ko");
    CHECK(flag(r, "driver.loaded"));
    CHECK(flag(r, "driver.device"));
    const auto& holders = r.root.at("driver.holders")->items();
    REQUIRE(holders.size() == 1);
    CHECK(holders[0].find("pid")->as_int() == 812);
    CHECK(holders[0].find("command")->as_string() == "radioberryd");
    CHECK(has(r, "driver.busy", Severity::kInfo));
    CHECK(str(r, "boot.spi0_0.driver") == "radioberry");
    CHECK(str(r, "fpga.gateware.version") == "75.3");
    CHECK(str(r, "fpga.gateware.fpga") == "10CL025");
    CHECK(r.root.at("fpga.gateware.receivers")->as_int() == 6);
    CHECK(r.root.at("fpga.gateware.transmitters")->as_int() == 1);
    CHECK(str(r, "fpga.type") == "10CL025");
    CHECK(str(r, "fpga.type_source") == "driver");
    CHECK_FALSE(has_id(r, "fpga.type"));
    CHECK(flag(r, "boot.overlay.active"));
    CHECK_FALSE(has_id(r, "boot.overlay"));
    CHECK(str(r, "firmware.family") == "rpi-5 PIO");
    CHECK(str(r, "software.daemon.version") == "radioberryd v0.1.0");
    CHECK(str(r, "software.daemon.service_active") == "active");
    CHECK_FALSE(flag(r, "fpga.config_lines.available"));
    CHECK(str(r, "fpga.config_lines.consumer") == "radioberry");

    CHECK(str(r, "software.daemon.path") == "/usr/local/sbin/radioberryd");
    // 1024 and DDC0's 1035 (0x040b) listed; 1065 (0x0429) is not ours.
    const auto& ports = r.root.at("software.udp_ports")->items();
    REQUIRE(ports.size() == 3);
    CHECK(ports[0].find("port")->as_int() == 1024);
    CHECK(ports[0].find("family")->as_string() == "ipv4");
    CHECK(ports[1].find("port")->as_int() == 1035);
    CHECK(ports[2].find("family")->as_string() == "ipv6");
}

TEST_CASE("driver-reported identity: CL016, odd values, mismatch", "[collect]") {
    FakeSource pi = working_pi();
    pi.files["/sys/bus/spi/devices/spi0.0/fpga_type"] = "10CL016\n";
    pi.files["/sys/bus/spi/devices/spi0.0/receivers"] = "many\n";
    pi.files.erase("/sys/bus/spi/devices/spi0.0/transmitters");
    Report r = collect(pi, test_options());
    CHECK(str(r, "fpga.type") == "10CL016");
    CHECK(r.root.at("fpga.gateware.receivers")->is_null());
    CHECK(r.root.at("fpga.gateware.transmitters")->is_null());
    // The installed image is for a CL025.
    CHECK(has(r, "firmware.fpga", Severity::kBlocker));

    pi.files["/sys/bus/spi/devices/spi0.0/fpga_type"] = "unknown\n";
    r = collect(pi, test_options());
    CHECK(r.root.at("fpga.type")->is_null());
    CHECK(str(r, "fpga.gateware.fpga") == "unknown");
    CHECK(has(r, "fpga.type", Severity::kInfo));
}

TEST_CASE("identify is refused while the driver is loaded", "[collect]") {
    FakeSource pi = working_pi();
    const Report r = collect(pi, test_options(true));
    CHECK(pi.configure_calls == 0);
    CHECK_FALSE(flag(r, "fpga.identify.attempted"));
    CHECK(has(r, "fpga.identify", Severity::kWarning));
    CHECK(str(r, "fpga.type_source") == "driver");
}

TEST_CASE("identify falls through to the CL016 image", "[collect]") {
    FakeSource pi = development_pi();
    pi.configure = [](std::span<const std::uint8_t> image) {
        const std::string content(reinterpret_cast<const char*>(image.data()), image.size());
        return content == kCl016Image ? ConfigResult{} : ConfigResult{ConfigError::kConfDoneLow};
    };
    const Report r = collect(pi, test_options(true));
    CHECK(pi.configure_calls == 2);
    CHECK(str(r, "fpga.type") == "10CL016");
    // The installed rpi-4 image is CL016 too: wrong family, right chip.
    CHECK_FALSE(has_id(r, "firmware.fpga"));
    CHECK(has(r, "firmware.family", Severity::kBlocker));
}

TEST_CASE("identify reports when no image configures the FPGA", "[collect]") {
    FakeSource pi = development_pi();
    pi.configure = nullptr;
    const Report r = collect(pi, test_options(true));
    CHECK(pi.configure_calls == 2);
    CHECK(r.root.at("fpga.type")->is_null());
    CHECK(has(r, "fpga.identify", Severity::kBlocker));
}

TEST_CASE("identify with no candidate images", "[collect]") {
    FakeSource pi = development_pi();
    Options o = test_options(true);
    o.gateware_dirs = {"/nowhere"};
    const Report r = collect(pi, o);
    CHECK(pi.configure_calls == 0);
    CHECK(has(r, "fpga.identify", Severity::kWarning));
}

TEST_CASE("identify finds images in the installed gateware layout", "[collect]") {
    FakeSource pi = development_pi();
    pi.files.erase("/srv/gateware/CL025/radioberry.rbf");
    pi.files.erase("/srv/gateware/CL016/radioberry.rbf");
    pi.files["/usr/share/radioberry/gateware/rpi-5/CL025/radioberry.rbf"] = kCl025Image;
    pi.files["/usr/share/radioberry/gateware/rpi-5/CL025/deeper/too-deep.rbf"] = kCl016Image;
    Options o = test_options(true);
    o.gateware_dirs = {"/usr/share/radioberry/gateware"};
    const Report r = collect(pi, o);
    CHECK(pi.configure_calls == 1);
    CHECK(str(r, "fpga.type") == "10CL025");
}

TEST_CASE("identify deduplicates identical images", "[collect]") {
    FakeSource pi = development_pi();
    pi.files["/lib/firmware/radioberry.rbf"] = kCl016Image;
    pi.files["/srv/gateware/copy.rbf"] = kCl016Image;
    pi.configure = nullptr;
    const Report r = collect(pi, test_options(true));
    CHECK(pi.configure_calls == 2);  // CL025 and CL016 once each
}

TEST_CASE("rpi-5 GPIO-mode gateware is installed but unusable", "[collect]") {
    static const std::string gpio_image = "fake rpi-5 GPIO CL025 gateware";
    static const std::string hash = radioberry::Sha256::hex(gpio_image);
    static const std::array<KnownImage, 1> catalog = {
        {{hash, GatewareFamily::kRpi5Gpio, FpgaType::k10CL025, gpio_image.size(), "archive", "t"}}};
    FakeSource pi = working_pi();
    pi.files["/lib/firmware/radioberry.rbf"] = gpio_image;
    pi.dirs.erase("/sys/module/radioberry");
    pi.files.erase("/dev/radioberry");
    pi.config_lines = {true, "", true, true};
    pi.files["/srv/gateware/gpio.rbf"] = gpio_image;
    pi.configure = [](std::span<const std::uint8_t>) { return ConfigResult{}; };
    Options o = test_options(true);
    o.catalog = catalog;
    const Report r = collect(pi, o);
    CHECK(str(r, "firmware.family") == "rpi-5 GPIO");
    CHECK(has(r, "firmware.family", Severity::kBlocker));
    // A GPIO-mode image still identifies the chip.
    CHECK(str(r, "fpga.type") == "10CL025");
}

TEST_CASE("missing and unknown firmware", "[collect]") {
    FakeSource pi = development_pi();
    pi.files.erase("/lib/firmware/radioberry.rbf");
    Report r = collect(pi, test_options());
    CHECK_FALSE(flag(r, "firmware.present"));
    CHECK(has(r, "firmware.missing", Severity::kBlocker));

    pi.files["/lib/firmware/radioberry.rbf"] = "something else";
    r = collect(pi, test_options());
    CHECK_FALSE(flag(r, "firmware.known"));
    CHECK(r.root.at("firmware.family")->is_null());
    CHECK(has(r, "firmware.unknown", Severity::kWarning));
}

TEST_CASE("boot problems are blockers", "[collect]") {
    FakeSource pi = development_pi();
    pi.files.erase("/proc/device-tree/radioberry/compatible");
    pi.files.erase("/proc/device-tree/radioberry/status");
    pi.files.erase("/sys/bus/spi/devices/spi0.0/modalias");
    pi.files.erase("/sys/bus/spi/devices/spi0.0/of_node/compatible");
    pi.dirs.erase("/sys/module/rp1_pio");
    pi.files.erase("/dev/i2c-1");
    pi.files["/dev/spidev0.0"] = "";
    pi.files["/dev/spidev10.0"] = "";
    const Report r = collect(pi, test_options());
    CHECK(has(r, "boot.overlay", Severity::kBlocker));
    CHECK_FALSE(flag(r, "boot.overlay.original"));
    CHECK(has(r, "boot.rp1_pio", Severity::kBlocker));
    CHECK(has(r, "boot.i2c", Severity::kWarning));
    CHECK(has(r, "boot.spidev", Severity::kBlocker));
    CHECK(r.root.at("boot.spidev")->items().size() == 2);
}

TEST_CASE("driver installed but not loaded, or loaded without a device", "[collect]") {
    FakeSource pi = working_pi();
    pi.dirs.erase("/sys/module/radioberry");
    Report r = collect(pi, test_options());
    CHECK(has(r, "driver.loaded", Severity::kBlocker));

    pi = working_pi();
    pi.files.erase("/dev/radioberry");
    pi.files.erase("/sys/bus/spi/devices/spi0.0/gateware_version");
    r = collect(pi, test_options());
    CHECK(has(r, "driver.device", Severity::kBlocker));
    CHECK(r.root.at("driver.holders")->items().empty());
    CHECK(has(r, "fpga.type", Severity::kInfo));
    CHECK(r.root.at("fpga.gateware")->is_null());
}

TEST_CASE("host details missing", "[collect]") {
    FakeSource s;
    const Report r = collect(s, test_options());
    CHECK(r.root.at("host.model")->is_null());
    CHECK(r.root.at("host.revision")->is_null());
    CHECK(r.root.at("host.os")->is_null());
    CHECK(r.root.at("host.kernel")->is_null());
    CHECK_FALSE(flag(r, "host.headers.present"));
    CHECK(has(r, "host.headers", Severity::kWarning));
    CHECK(r.root.at("host.bootloader.date")->is_null());
    CHECK(r.root.at("host.pmic")->is_null());
    CHECK(r.root.at("host.throttled")->is_null());
    CHECK(r.root.at("pa.devices")->is_null());
    CHECK(r.root.at("software.liquid_dsp")->is_null());
    CHECK(r.root.at("software.daemon.service_enabled")->is_null());
    CHECK(r.root.at("driver.module_file")->is_null());
    CHECK_FALSE(r.radio_can_run());
}

TEST_CASE("bootloader output without a version line", "[collect]") {
    FakeSource pi = development_pi();
    pi.commands[{"vcgencmd", "bootloader_version"}] = {0, ""};
    const Report r = collect(pi, test_options());
    CHECK(r.root.at("host.bootloader.date")->is_null());
    CHECK(r.root.at("host.bootloader.version")->is_null());
}

TEST_CASE("throttle flags", "[collect]") {
    FakeSource pi = development_pi();
    pi.commands[{"vcgencmd", "get_throttled"}] = {0, "throttled=0x50005\n"};
    Report r = collect(pi, test_options());
    const auto& flags = r.root.at("host.throttled.flags")->items();
    REQUIRE(flags.size() == 4);
    CHECK(flags[0].as_string() == "under-voltage");
    CHECK(flags[1].as_string() == "throttled");
    CHECK(flags[2].as_string() == "under-voltage-occurred");
    CHECK(flags[3].as_string() == "throttling-occurred");
    CHECK(has(r, "host.throttled", Severity::kWarning));

    pi.commands[{"vcgencmd", "get_throttled"}] = {0, "throttled=0x80000\n"};
    r = collect(pi, test_options());
    CHECK(has(r, "host.throttled", Severity::kInfo));

    pi.commands[{"vcgencmd", "get_throttled"}] = {0, "throttled=zz\n"};
    r = collect(pi, test_options());
    CHECK(r.root.at("host.throttled")->is_null());

    pi.commands[{"vcgencmd", "get_throttled"}] = {1, ""};
    r = collect(pi, test_options());
    CHECK(r.root.at("host.throttled")->is_null());
}

TEST_CASE("PA classification by I2C address", "[collect]") {
    FakeSource pi = development_pi();
    auto board = [&](std::vector<I2cDevice> devices) {
        pi.i2c = std::move(devices);
        return str(collect(pi, test_options()), "pa.board");
    };
    CHECK(board({{0x34, false}, {0x2c, false}}) ==
          "Radioberry 5 W preamp: digital bias and telemetry");
    CHECK(board({{0x34, false}}) == "PA with current and temperature telemetry, manual bias");
    CHECK(board({{0x2c, false}}) == "PA with digital bias, no telemetry");

    pi.i2c = std::vector<I2cDevice>{{0x20, false}, {0x50, true}};
    const Report r = collect(pi, test_options());
    const auto& devices = r.root.at("pa.devices")->items();
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].find("address")->as_string() == "0x20");
    CHECK(devices[0].find("device")->as_string() == "N2ADR filter board (MCP23008)");
    CHECK(devices[1].find("device")->as_string() == "unknown");
    CHECK(devices[1].find("claimed_by_driver")->as_bool());
    CHECK(flag(r, "pa.filter_board"));
    CHECK_FALSE(flag(r, "pa.telemetry"));
}

TEST_CASE("dpkg status parsing ignores removed packages", "[collect]") {
    FakeSource pi = development_pi();
    pi.files["/var/lib/dpkg/status"] =
        "Package: libliquid2\nStatus: deinstall ok config-files\nVersion: 2.0\n\n"
        "Package: libliquid1\nStatus: install ok not-installed\nVersion: 1.5\n\n"
        "Package: other\nStatus: install ok installed\nVersion: 9\n\n"
        "Package: libliquid1\nStatus: install ok installed\nVersion: 1.7.0-1\n";
    const Report r = collect(pi, test_options());
    CHECK(str(r, "software.liquid_dsp") == "1.7.0-1");
}

TEST_CASE("severity names", "[collect]") {
    CHECK(to_string(Severity::kBlocker) == "blocker");
    CHECK(to_string(Severity::kWarning) == "warning");
    CHECK(to_string(Severity::kInfo) == "info");
}
