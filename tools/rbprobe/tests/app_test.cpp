// SPDX-License-Identifier: GPL-2.0-or-later
#include "app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <sstream>

#include "fake_source.hpp"

using namespace radioberry::probe;
using namespace radioberry::probe::test;

namespace {

struct Run {
    int code;
    std::string out;
    std::string err;
};

Run run(std::vector<std::string> args, FakeSource source) {
    std::ostringstream out;
    std::ostringstream err;
    const int code = run_app(args, source, out, err);
    return {code, out.str(), err.str()};
}

}  // namespace

TEST_CASE("exit status follows the verdict", "[app]") {
    CHECK(run({}, development_pi()).code == kExitRadioCannotRun);
    // The default catalog does not know the fake images, so the working Pi
    // reports an unknown image (a warning) and can still run.
    CHECK(run({}, working_pi()).code == kExitRadioCanRun);
}

TEST_CASE("output formats", "[app]") {
    const Run text = run({}, development_pi());
    CHECK(text.out.find("radio can run: no") != std::string::npos);

    const Run json = run({"--json"}, development_pi());
    const auto j = nlohmann::json::parse(json.out);
    CHECK(j["radio_can_run"] == false);
    CHECK(j["host"]["model"] == "Raspberry Pi 5 Model B Rev 1.0");

    const Run yaml = run({"--yaml"}, development_pi());
    CHECK(yaml.out.find("radio_can_run: false") != std::string::npos);
}

TEST_CASE("text output lists findings and the verdict", "[app]") {
    const Run text = run({}, development_pi());
    CHECK(text.out.find("host\n") == 0);
    CHECK(text.out.find("\nfindings\n  blocker ") != std::string::npos);
    CHECK(text.out.find("radio_can_run") == std::string::npos);
}

TEST_CASE("probe text with no findings and with a malformed one", "[app]") {
    radioberry::Node root = radioberry::Node::map();
    root.set("findings", radioberry::Node::seq());
    root.set("radio_can_run", true);
    std::string text = render_probe_text(root);
    CHECK(text.find("findings\n  none\n") != std::string::npos);
    CHECK(text.find("radio can run: yes") != std::string::npos);

    radioberry::Node bad = radioberry::Node::map();
    radioberry::Node findings = radioberry::Node::seq();
    findings.push(radioberry::Node::map());
    bad.set("findings", findings);
    text = render_probe_text(bad);
    CHECK(text.find("findings\n   : \n") != std::string::npos);
    CHECK(text.find("radio can run") == std::string::npos);
}

TEST_CASE("identify options", "[app]") {
    FakeSource pi = development_pi();
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string> args = {"--json",        "--identify",     "--gateware-dir",
                                           "/srv/gateware", "--gateware-dir", "/elsewhere"};
    run_app(args, pi, out, err);
    // Default catalog: the fake images are unknown, so nothing is tried.
    CHECK(pi.configure_calls == 0);
    const auto j = nlohmann::json::parse(out.str());
    CHECK(j["fpga"]["identify"]["attempted"] == true);
}

TEST_CASE("usage errors", "[app]") {
    const Run bad = run({"--bogus"}, development_pi());
    CHECK(bad.code == kExitUsage);
    CHECK(bad.err.find("unknown option --bogus") != std::string::npos);

    const Run missing = run({"--gateware-dir"}, development_pi());
    CHECK(missing.code == kExitUsage);
    CHECK(missing.err.find("needs a directory") != std::string::npos);
}

TEST_CASE("help and version", "[app]") {
    const Run help = run({"--help"}, development_pi());
    CHECK(help.code == 0);
    CHECK(help.out.find("usage: rbprobe") == 0);
    CHECK(run({"-h"}, development_pi()).out.find("usage") == 0);

    const Run version = run({"--version"}, development_pi());
    CHECK(version.code == 0);
    CHECK(version.out.find("rbprobe ") == 0);
}
