// SPDX-License-Identifier: GPL-2.0-or-later
#include "radioberry/render.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <sstream>

namespace radioberry {
namespace {

using Json = nlohmann::ordered_json;

Json to_json(const Node& node) {
    switch (node.kind()) {
        case Node::Kind::kNull:
            return nullptr;
        case Node::Kind::kBool:
            return node.as_bool();
        case Node::Kind::kInt:
            return node.as_int();
        case Node::Kind::kDouble:
            return node.as_double();
        case Node::Kind::kString:
            return node.as_string();
        case Node::Kind::kMap: {
            Json j = Json::object();
            for (const auto& [key, value] : node.entries()) {
                j[key] = to_json(value);
            }
            return j;
        }
        case Node::Kind::kSeq:
            break;
    }
    Json j = Json::array();
    for (const auto& item : node.items()) {
        j.push_back(to_json(item));
    }
    return j;
}

void emit(YAML::Emitter& out, const Node& node) {
    switch (node.kind()) {
        case Node::Kind::kNull:
            out << YAML::Null;
            break;
        case Node::Kind::kBool:
            out << node.as_bool();
            break;
        case Node::Kind::kInt:
            out << node.as_int();
            break;
        case Node::Kind::kDouble:
            // Plain scalar in shortest form; yaml-cpp's own double output
            // prints every digit of the binary value.
            out << format_double(node.as_double());
            break;
        case Node::Kind::kString:
            out << YAML::DoubleQuoted << node.as_string();
            break;
        case Node::Kind::kMap:
            if (node.entries().empty()) {
                out << YAML::Flow << YAML::BeginMap << YAML::EndMap;
                break;
            }
            out << YAML::BeginMap;
            for (const auto& [key, value] : node.entries()) {
                out << YAML::Key << key << YAML::Value;
                emit(out, value);
            }
            out << YAML::EndMap;
            break;
        case Node::Kind::kSeq:
            if (node.items().empty()) {
                out << YAML::Flow << YAML::BeginSeq << YAML::EndSeq;
                break;
            }
            out << YAML::BeginSeq;
            for (const auto& item : node.items()) {
                emit(out, item);
            }
            out << YAML::EndSeq;
            break;
    }
}

bool is_empty(const Node& n) { return !n.is_scalar() && n.entries().empty() && n.items().empty(); }

void text_node(std::ostringstream& out, const Node& node, int depth);

void text_map(std::ostringstream& out, const Node& node, int depth) {
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    std::size_t width = 0;
    for (const auto& [key, value] : node.entries()) {
        if (value.is_scalar() || is_empty(value)) {
            width = std::max(width, key.size());
        }
    }
    for (const auto& [key, value] : node.entries()) {
        if (value.is_scalar()) {
            out << indent << key << std::string(width - key.size() + 2, ' ') << scalar_text(value)
                << '\n';
        } else if (is_empty(value)) {
            out << indent << key << std::string(width - key.size() + 2, ' ') << "none\n";
        } else {
            out << indent << key << '\n';
            text_node(out, value, depth + 1);
        }
    }
}

void text_node(std::ostringstream& out, const Node& node, int depth) {
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    if (node.kind() == Node::Kind::kMap) {
        text_map(out, node, depth);
    } else if (node.kind() == Node::Kind::kSeq) {
        for (const auto& item : node.items()) {
            if (item.is_scalar()) {
                out << indent << "- " << scalar_text(item) << '\n';
            } else {
                // One line per item: "- key value, key value".
                out << indent << "-";
                bool first = true;
                for (const auto& [key, value] : item.entries()) {
                    out << (first ? " " : ", ") << key << ' '
                        << (value.is_scalar() ? scalar_text(value) : "...");
                    first = false;
                }
                out << '\n';
            }
        }
    } else {
        out << indent << scalar_text(node) << '\n';
    }
}

}  // namespace

std::string scalar_text(const Node& node) {
    switch (node.kind()) {
        case Node::Kind::kNull:
            return "-";
        case Node::Kind::kBool:
            return node.as_bool() ? "yes" : "no";
        case Node::Kind::kInt:
            return std::to_string(node.as_int());
        case Node::Kind::kDouble:
            return format_double(node.as_double());
        case Node::Kind::kString:
            return node.as_string();
        case Node::Kind::kMap:
        case Node::Kind::kSeq:
            break;
    }
    return {};
}

std::string render_json(const Node& root) { return to_json(root).dump(2) + "\n"; }

std::string render_yaml(const Node& root) {
    YAML::Emitter out;
    emit(out, root);
    return std::string(out.c_str()) + "\n";
}

std::string render_text(const Node& root) {
    std::ostringstream out;
    bool first = true;
    for (const auto& [key, value] : root.entries()) {
        if (!first) {
            out << '\n';
        }
        first = false;
        out << key << '\n';
        if (is_empty(value)) {
            out << "  none\n";
        } else {
            text_node(out, value, 1);
        }
    }
    return out.str();
}

}  // namespace radioberry
