// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

using namespace radioberry;
using namespace radioberry::load;

namespace {

std::string committed(const std::string& relative) {
    std::ifstream in(std::string(RADIOBERRY_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

// Configuration lines of an FPGA that accepts any bitstream, or none.
class FakePins final : public ConfigPins {
public:
    explicit FakePins(bool accept, long* clocks) : accept_(accept), clocks_(clocks) {}
    void set_nconfig(bool high) override { nstatus_ = high; }
    void set_data(bool) override {}
    void set_dclk(bool high) override {
        if (high) {
            ++*clocks_;
        }
    }
    bool nstatus() override { return nstatus_; }
    bool conf_done() override { return accept_; }
    void delay(std::chrono::microseconds) override {}

private:
    bool accept_;
    long* clocks_;
    bool nstatus_ = true;
};

struct Harness {
    std::map<std::string, std::string> files;
    bool loaded = false;
    bool accept = true;
    bool pins_fail = false;
    int opened = 0;
    long clocks = 0;
    std::ostringstream out;
    std::ostringstream err;

    int run(std::vector<std::string> args) {
        Environment env{
            .read_file =
                [this](const std::string& path) {
                    const auto it = files.find(path);
                    if (it == files.end()) {
                        throw std::runtime_error("cannot read " + path);
                    }
                    return it->second;
                },
            .driver_loaded = [this] { return loaded; },
            .open_pins = [this]() -> std::unique_ptr<ConfigPins> {
                ++opened;
                if (pins_fail) {
                    throw std::runtime_error("cannot request FPGA configuration lines");
                }
                return std::make_unique<FakePins>(accept, &clocks);
            },
            .out = out,
            .err = err,
        };
        return run_app(args, env);
    }
};

}  // namespace

TEST_CASE("rbload parses its options", "[rbload]") {
    const auto parse = [](std::vector<std::string> args) { return parse_options(args); };
    const Options o = std::get<Options>(parse({}));
    CHECK(o.image == kDefaultImage);
    CHECK_FALSE(o.fpga);
    CHECK_FALSE(o.force);

    const Options g = std::get<Options>(parse({"--fpga", "CL016", "--force", "--json", "x.rbf"}));
    CHECK(g.image == "x.rbf");
    CHECK(g.fpga == FpgaType::k10CL016);
    CHECK(g.force);
    CHECK(g.format == ReportFormat::kJson);
    CHECK(std::get<Options>(parse({"--fpga", "10CL025", "--yaml"})).fpga == FpgaType::k10CL025);
    CHECK(std::get<Options>(parse({"--yaml"})).format == ReportFormat::kYaml);

    CHECK(std::holds_alternative<ShowHelp>(parse({"--help"})));
    CHECK(std::holds_alternative<ShowVersion>(parse({"--version"})));
    CHECK(std::holds_alternative<UsageError>(parse({"--fpga"})));
    CHECK(std::holds_alternative<UsageError>(parse({"--fpga", "CL040"})));
    CHECK(std::holds_alternative<UsageError>(parse({"--bogus"})));
    CHECK(std::holds_alternative<UsageError>(parse({"a.rbf", "b.rbf"})));
}

TEST_CASE("rbload loads this project's image and reports it", "[rbload]") {
    Harness h;
    h.files[kDefaultImage] = committed("gateware/rpi-5/CL025/radioberry.rbf");
    REQUIRE(h.files[kDefaultImage].size() == 372888);
    CHECK(h.run({"--fpga", "CL025"}) == kExitOk);
    CHECK(h.opened == 1);
    // Every bit clocked in, then the initialisation clocks.
    CHECK(h.clocks == 372888L * 8 + 64);
    const std::string text = h.out.str();
    CHECK(text.find("rpi-5 PIO") != std::string::npos);
    CHECK(text.find("10CL025") != std::string::npos);
    CHECK(text.find("configured") != std::string::npos);
    CHECK(h.err.str().empty());

    Harness j;
    j.files["cl016.rbf"] = committed("gateware/rpi-5/CL016/radioberry.rbf");
    CHECK(j.run({"--json", "cl016.rbf"}) == kExitOk);
    CHECK(j.out.str().find("\"fpga\": \"10CL016\"") != std::string::npos);
    CHECK(j.out.str().find("\"configured\": true") != std::string::npos);

    Harness y;
    y.files[kDefaultImage] = committed("gateware/rpi-5/CL025/radioberry.rbf");
    CHECK(y.run({"--yaml"}) == kExitOk);
    CHECK(y.out.str().find("configured: true") != std::string::npos);
}

TEST_CASE("rbload reports an FPGA that does not configure", "[rbload]") {
    Harness h;
    h.accept = false;
    h.files[kDefaultImage] = committed("gateware/rpi-5/CL025/radioberry.rbf");
    CHECK(h.run({}) == kExitFailed);
    CHECK(h.out.str().find("CONF_DONE stayed low") != std::string::npos);
}

TEST_CASE("rbload refuses images it should not load", "[rbload]") {
    Harness h;
    h.files["mine.rbf"] = "not a known image";
    CHECK(h.run({"mine.rbf"}) == kExitUsage);
    CHECK(h.err.str().find("not one rbprobe knows") != std::string::npos);
    CHECK(h.opened == 0);
    // --force loads it.
    CHECK(h.run({"--force", "mine.rbf"}) == kExitOk);
    CHECK(h.opened == 1);
    CHECK(h.out.str().find("known") != std::string::npos);

    Harness wrong;
    wrong.files[kDefaultImage] = committed("gateware/rpi-5/CL025/radioberry.rbf");
    CHECK(wrong.run({"--fpga", "CL016"}) == kExitUsage);
    CHECK(wrong.err.str().find("is for a 10CL025, not a 10CL016") != std::string::npos);
    // --force does not override the FPGA check.
    CHECK(wrong.run({"--fpga", "CL016", "--force"}) == kExitUsage);
    CHECK(wrong.opened == 0);
}

TEST_CASE("rbload refuses other image families unless forced", "[rbload]") {
    const std::optional<KnownImage> gpio =
        find_known_image("fe6a4e8acb67fbf80aa679affc8f2e0e0abe15908938cb2aed10804b461105cd");
    REQUIRE(gpio);
    Options o;
    const auto why = refusal(o, gpio);
    REQUIRE(why);
    CHECK(why->find("rpi-5 GPIO") != std::string::npos);
    o.force = true;
    CHECK_FALSE(refusal(o, gpio));
}

TEST_CASE("rbload stops while the driver is loaded or the lines are busy", "[rbload]") {
    Harness h;
    h.loaded = true;
    h.files[kDefaultImage] = committed("gateware/rpi-5/CL025/radioberry.rbf");
    CHECK(h.run({}) == kExitUsage);
    CHECK(h.err.str().find("rmmod radioberry") != std::string::npos);
    CHECK(h.opened == 0);

    Harness busy;
    busy.pins_fail = true;
    busy.files[kDefaultImage] = h.files[kDefaultImage];
    CHECK(busy.run({}) == kExitFailed);
    CHECK(busy.err.str().find("configuration lines") != std::string::npos);
}

TEST_CASE("rbload reports an unreadable image and usage", "[rbload]") {
    Harness h;
    CHECK(h.run({"missing.rbf"}) == kExitFailed);
    CHECK(h.err.str().find("cannot read missing.rbf") != std::string::npos);

    Harness help;
    CHECK(help.run({"--help"}) == kExitOk);
    CHECK(help.out.str() == kUsage);
    Harness version;
    CHECK(version.run({"--version"}) == kExitOk);
    CHECK(version.out.str().starts_with("rbload "));
    Harness bad;
    CHECK(bad.run({"--bogus"}) == kExitUsage);
    CHECK(bad.err.str().starts_with("rbload: unknown option --bogus"));
}
