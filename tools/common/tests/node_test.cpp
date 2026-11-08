// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/node.hpp"

#include <catch2/catch_test_macros.hpp>

using radioberry::Node;

TEST_CASE("node scalars", "[node]") {
    CHECK(Node().is_null());
    CHECK(Node(true).as_bool());
    CHECK(Node(42).as_int() == 42);
    CHECK(Node(7L).as_int() == 7);
    CHECK(Node(8LL).as_int() == 8);
    CHECK(Node(9U).as_int() == 9);
    CHECK(Node(10UL).as_int() == 10);
    CHECK(Node(11ULL).as_int() == 11);
    CHECK(Node(2.5).as_double() == 2.5);
    CHECK(Node(2.5).kind() == Node::Kind::kDouble);
    CHECK(Node(1).is_scalar());
    CHECK_FALSE(Node::map().is_scalar());
    CHECK(Node("text").as_string() == "text");
    CHECK(Node(std::string("s")).kind() == Node::Kind::kString);
    CHECK(Node(std::optional<std::string>{}).is_null());
    CHECK(Node(std::optional<std::string>{"x"}).as_string() == "x");
}

TEST_CASE("node maps keep insertion order and replace keys", "[node]") {
    Node m = Node::map();
    CHECK(m.kind() == Node::Kind::kMap);
    m.set("b", 1);
    m.set("a", 2);
    m.set("b", 3);
    REQUIRE(m.entries().size() == 2);
    CHECK(m.entries()[0].first == "b");
    CHECK(m.entries()[0].second.as_int() == 3);
    CHECK(m.find("a")->as_int() == 2);
    CHECK(m.find("zz") == nullptr);
    CHECK(Node(1).find("a") == nullptr);
}

TEST_CASE("node path lookup", "[node]") {
    Node root = Node::map();
    Node inner = Node::map();
    inner.set("c", "deep");
    Node mid = Node::map();
    mid.set("b", inner);
    root.set("a", mid);
    CHECK(root.at("a.b.c")->as_string() == "deep");
    CHECK(root.at("a.x.c") == nullptr);
    CHECK(root.at("a") != nullptr);
    CHECK(root.at("a.b.c.d") == nullptr);
}

TEST_CASE("node sequences", "[node]") {
    Node s = Node::seq();
    CHECK(s.kind() == Node::Kind::kSeq);
    s.push(1);
    s.push("two");
    REQUIRE(s.items().size() == 2);
    CHECK(s.items()[1].as_string() == "two");
    CHECK(s == s);
    CHECK_FALSE(s == Node::seq());
}

TEST_CASE("node without keys", "[node]") {
    Node m = Node::map();
    m.set("a", 1);
    m.set("b", 2);
    m.set("c", 3);
    const Node rest = m.without({"b", "zz"});
    REQUIRE(rest.entries().size() == 2);
    CHECK(rest.entries()[0].first == "a");
    CHECK(rest.entries()[1].first == "c");
    CHECK(Node::map().without({"a"}).entries().empty());
}

TEST_CASE("doubles format in shortest form", "[node]") {
    CHECK(radioberry::format_double(48000.0) == "48000");
    CHECK(radioberry::format_double(-6.5) == "-6.5");
    CHECK(radioberry::format_double(0.1) == "0.1");
}
