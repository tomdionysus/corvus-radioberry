// SPDX-License-Identifier: GPL-2.0-or-later
#include "node.hpp"

namespace radioberry::probe {

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

}  // namespace radioberry::probe
