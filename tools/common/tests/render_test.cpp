// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/render.hpp"

#include <yaml-cpp/yaml.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace radioberry;

namespace {

Node sample() {
    Node root = Node::map();
    Node host = Node::map();
    host.set("model", "Raspberry Pi 5");
    host.set("cores", 4);
    host.set("serial", Node());
    host.set("headers_ok", true);
    host.set("load", 0.25);
    host.set("empty_map", Node::map());
    Node nets = Node::seq();
    Node n = Node::map();
    n.set("name", "wlan0");
    Node nested = Node::map();
    nested.set("x", 1);
    n.set("nested", nested);
    nets.push(n);
    nets.push("scalar item");
    host.set("network", nets);
    host.set("none_list", Node::seq());
    root.set("host", host);
    root.set("version", "0.0.1");
    Node findings = Node::seq();
    Node f = Node::map();
    f.set("severity", "blocker");
    f.set("id", "driver.module");
    f.set("message", "no module");
    findings.push(f);
    root.set("findings", findings);
    root.set("radio_can_run", false);
    return root;
}

// Converts parsed JSON or YAML back into a Node-equivalent JSON value so
// the two outputs can be compared.
nlohmann::ordered_json from_yaml(const YAML::Node& y) {
    if (y.IsNull()) {
        return nullptr;
    }
    if (y.IsMap()) {
        nlohmann::ordered_json j = nlohmann::ordered_json::object();
        for (const auto& kv : y) {
            j[kv.first.as<std::string>()] = from_yaml(kv.second);
        }
        return j;
    }
    if (y.IsSequence()) {
        nlohmann::ordered_json j = nlohmann::ordered_json::array();
        for (const auto& item : y) {
            j.push_back(from_yaml(item));
        }
        return j;
    }
    if (y.Tag() == "!") {
        return y.as<std::string>();  // double-quoted string
    }
    bool b = false;
    if (YAML::convert<bool>::decode(y, b)) {
        return b;
    }
    long long i = 0;
    if (YAML::convert<long long>::decode(y, i)) {
        return i;
    }
    return y.as<double>();
}

}  // namespace

TEST_CASE("JSON and YAML carry identical content", "[render]") {
    const Node root = sample();
    const auto json = nlohmann::ordered_json::parse(render_json(root));
    const auto yaml = from_yaml(YAML::Load(render_yaml(root)));
    CHECK(json == yaml);
    CHECK(json["host"]["model"] == "Raspberry Pi 5");
    CHECK(json["host"]["serial"].is_null());
    CHECK(json["radio_can_run"] == false);
    CHECK(json["host"]["empty_map"].is_object());
    CHECK(json["host"]["none_list"].is_array());
    CHECK(json["host"]["load"] == 0.25);
}

TEST_CASE("YAML quotes strings that look like other types", "[render]") {
    Node root = Node::map();
    root.set("looks_bool", "yes");
    root.set("looks_int", "1024");
    root.set("looks_null", "~");
    const YAML::Node y = YAML::Load(render_yaml(root));
    CHECK(y["looks_bool"].as<std::string>() == "yes");
    CHECK(y["looks_bool"].Tag() == "!");
    CHECK(y["looks_int"].Tag() == "!");
    CHECK(y["looks_null"].as<std::string>() == "~");
}

TEST_CASE("text output lists sections", "[render]") {
    const std::string text = render_text(sample());
    CHECK(text.find("host\n") == 0);
    CHECK(text.find("  model       Raspberry Pi 5\n") != std::string::npos);
    CHECK(text.find("  serial      -\n") != std::string::npos);
    CHECK(text.find("  headers_ok  yes\n") != std::string::npos);
    CHECK(text.find("  load        0.25\n") != std::string::npos);
    CHECK(text.find("  empty_map   none\n") != std::string::npos);
    CHECK(text.find("    - name wlan0, nested ...\n") != std::string::npos);
    CHECK(text.find("    - scalar item\n") != std::string::npos);
    CHECK(text.find("version\n  0.0.1\n") != std::string::npos);
    CHECK(text.find("findings\n  - severity blocker, id driver.module, message no module\n") !=
          std::string::npos);
    CHECK(text.find("radio_can_run\n  no\n") != std::string::npos);
}

TEST_CASE("text output of empty and nested top-level values", "[render]") {
    Node root = Node::map();
    Node outer = Node::map();
    Node inner = Node::map();
    inner.set("k", "v");
    outer.set("inner", inner);
    root.set("outer", outer);
    root.set("nothing", Node::seq());
    const std::string text = render_text(root);
    CHECK(text.find("outer\n  inner\n    k  v\n") != std::string::npos);
    CHECK(text.find("nothing\n  none\n") != std::string::npos);
}

TEST_CASE("scalar text", "[render]") {
    CHECK(scalar_text(Node()) == "-");
    CHECK(scalar_text(Node(false)) == "no");
    CHECK(scalar_text(Node(-3)) == "-3");
    CHECK(scalar_text(Node(1.5)) == "1.5");
    CHECK(scalar_text(Node("s")) == "s");
    CHECK(scalar_text(Node::map()).empty());
    CHECK(scalar_text(Node::seq()).empty());
}
