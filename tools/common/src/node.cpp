// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/node.hpp"

#include <algorithm>
#include <array>
#include <charconv>

namespace radioberry {

Node& Node::set(const std::string& key, Node value) {
    kind_ = Kind::kMap;
    for (auto& entry : map_) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return entry.second;
        }
    }
    map_.emplace_back(key, std::move(value));
    return map_.back().second;
}

Node& Node::push(Node value) {
    kind_ = Kind::kSeq;
    seq_.push_back(std::move(value));
    return seq_.back();
}

const Node* Node::find(const std::string& key) const {
    if (kind_ != Kind::kMap) {
        return nullptr;
    }
    for (const auto& entry : map_) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

const Node* Node::at(const std::string& path) const {
    const Node* node = this;
    std::size_t start = 0;
    while (node != nullptr && start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string key =
            path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        node = node->find(key);
        if (dot == std::string::npos) {
            return node;
        }
        start = dot + 1;
    }
    return node;
}

Node Node::without(const std::vector<std::string>& keys) const {
    Node copy = Node::map();
    for (const auto& [key, value] : map_) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            copy.set(key, value);
        }
    }
    return copy;
}

std::string format_double(double value) {
    std::array<char, 32> buf{};
    const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), result.ptr);
}

}  // namespace radioberry
