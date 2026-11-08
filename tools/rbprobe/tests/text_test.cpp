// SPDX-License-Identifier: GPL-2.0-or-later
#include "text.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace radioberry::probe;

TEST_CASE("trim strips whitespace and NUL", "[text]") {
    CHECK(trim("  a b \n") == "a b");
    CHECK(trim(std::string("okay") + '\0') == "okay");
    CHECK(trim("   ") == "");
    CHECK(trim("") == "");
}

TEST_CASE("split lines and words", "[text]") {
    CHECK(split_lines("a\nb\n") == std::vector<std::string>{"a", "b"});
    CHECK(split_lines("a\n\nc") == std::vector<std::string>{"a", "", "c"});
    CHECK(split_lines("").empty());
    CHECK(split_words("  x  y\tz ") == std::vector<std::string>{"x", "y", "z"});
    CHECK(split_words("   ").empty());
}

TEST_CASE("basename", "[text]") {
    CHECK(base_name("/a/b/c.ko") == "c.ko");
    CHECK(base_name("plain") == "plain");
}

TEST_CASE("key value and unquote", "[text]") {
    const std::string text = "NAME=\"Debian\"\nPRETTY_NAME=\"Debian 13\"\nID=debian\n";
    CHECK(unquote(key_value(text, "PRETTY_NAME")) == "Debian 13");
    CHECK(key_value(text, "ID") == "debian");
    CHECK(key_value(text, "MISSING").empty());
    CHECK(unquote("'single'") == "single");
    CHECK(unquote("\"") == "\"");
    CHECK(unquote("\"mismatch'") == "\"mismatch'");
}
