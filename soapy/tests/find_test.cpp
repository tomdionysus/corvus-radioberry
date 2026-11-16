// SPDX-License-Identifier: GPL-2.0-or-later
#include "find.hpp"

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>

using namespace radioberry::soapy;

namespace {

// A directory of its own under the system's temporary directory.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("rb-soapy-find-" + std::to_string(::getpid()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::string file(const std::string& name, const std::string& text) const {
        std::ofstream(path / name) << text;
        return (path / name).string();
    }
};

}  // namespace

TEST_CASE("find lists the radio when its device node exists") {
    const TempDir dir;
    const std::string device = dir.file("radioberry", "");

    SECTION("with the driver's identity") {
        dir.file("transport", "rp1-pio\n");
        dir.file("gateware_version", "75.3\n");
        const auto found = find_devices({{"device", device}}, dir.path.string());
        REQUIRE(found.size() == 1);
        CHECK(found[0].at("driver") == "radioberry");
        CHECK(found[0].at("device") == device);
        CHECK(found[0].at("label") == "RadioBerry (rp1-pio, gateware 75.3)");
    }
    SECTION("without it") {
        const auto found =
            find_devices({{"driver", "radioberry"}, {"device", device}}, dir.path.string());
        REQUIRE(found.size() == 1);
        CHECK(found[0].at("label") == "RadioBerry");
    }
    SECTION("not for another driver") {
        CHECK(find_devices({{"driver", "rtlsdr"}, {"device", device}}).empty());
    }
    SECTION("not without the node") {
        CHECK(find_devices({{"device", (dir.path / "absent").string()}}).empty());
    }
}

TEST_CASE("find looks at /dev/radioberry by default") {
    const auto found = find_devices({});
    CHECK(found.size() == (std::filesystem::exists("/dev/radioberry") ? 1u : 0u));
}

TEST_CASE("the gateware's TX sample count from the driver's counters") {
    const TempDir dir;
    CHECK_FALSE(image_tx_samples(dir.path.string()).has_value());
    dir.file("counters",
             "words_to_pi 0x02e4745a 48526426\ntx_samples 0x00024306 148230\n"
             "driver_tx_late 0\n");
    CHECK(image_tx_samples(dir.path.string()) == 148230u);
    dir.file("counters", "driver_tx_late 0\nwords_to_pi 0x1 1\n");
    CHECK_FALSE(image_tx_samples(dir.path.string()).has_value());
    CHECK(image_tx_samples("/nonexistent").has_value() == false);
}

TEST_CASE("open errors say what to do") {
    CHECK(open_error("/dev/radioberry", EBUSY).find("systemctl stop radioberry") !=
          std::string::npos);
    CHECK(open_error("/dev/radioberry", EACCES).find("radioberry group") != std::string::npos);
    CHECK(open_error("/dev/radioberry", ENOENT).find("dtoverlay=radioberry") != std::string::npos);
    CHECK(open_error("/dev/radioberry", EIO) == "/dev/radioberry: Input/output error");
}
