// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace radioberry {

// An ordered document tree. A tool's report is built once as a Node and
// rendered to text, JSON and YAML, so all three carry identical content.
class Node {
public:
    enum class Kind { kNull, kBool, kInt, kDouble, kString, kMap, kSeq };
    using Entry = std::pair<std::string, Node>;

    Node() = default;
    Node(bool value) : kind_(Kind::kBool), bool_(value) {}
    Node(int value) : kind_(Kind::kInt), int_(value) {}
    Node(long value) : kind_(Kind::kInt), int_(value) {}
    Node(long long value) : kind_(Kind::kInt), int_(value) {}
    Node(unsigned value) : kind_(Kind::kInt), int_(value) {}
    Node(unsigned long value) : kind_(Kind::kInt), int_(static_cast<std::int64_t>(value)) {}
    Node(unsigned long long value) : kind_(Kind::kInt), int_(static_cast<std::int64_t>(value)) {}
    Node(double value) : kind_(Kind::kDouble), double_(value) {}
    Node(std::string value) : kind_(Kind::kString), string_(std::move(value)) {}
    Node(const char* value) : kind_(Kind::kString), string_(value) {}

    template <typename T>
    Node(const std::optional<T>& value) {
        if (value) {
            *this = Node(*value);
        }
    }

    static Node map() {
        Node n;
        n.kind_ = Kind::kMap;
        return n;
    }
    static Node seq() {
        Node n;
        n.kind_ = Kind::kSeq;
        return n;
    }

    // Map: add or replace a key, keeping first-insertion order.
    Node& set(const std::string& key, Node value);
    // Sequence: append.
    Node& push(Node value);

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::kNull; }
    bool is_scalar() const { return kind_ != Kind::kMap && kind_ != Kind::kSeq; }
    bool as_bool() const { return bool_; }
    std::int64_t as_int() const { return int_; }
    double as_double() const { return double_; }
    const std::string& as_string() const { return string_; }
    const std::vector<Entry>& entries() const { return map_; }
    const std::vector<Node>& items() const { return seq_; }

    // Map lookup; nullptr if absent or not a map.
    const Node* find(const std::string& key) const;
    // Path lookup through maps, "a.b.c".
    const Node* at(const std::string& path) const;
    // A copy of a map without the named keys.
    Node without(const std::vector<std::string>& keys) const;

    bool operator==(const Node& other) const = default;

private:
    Kind kind_ = Kind::kNull;
    bool bool_ = false;
    std::int64_t int_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::vector<Entry> map_;
    std::vector<Node> seq_;
};

// Shortest text that reads back as the same double ("48000", "-6.5").
std::string format_double(double value);

}  // namespace radioberry
