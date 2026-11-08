// SPDX-License-Identifier: GPL-2.0-or-later
#include "text.hpp"

namespace radioberry::probe {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0' || c == '\v' || c == '\f';
}

}  // namespace

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

std::vector<std::string> split_words(std::string_view text) {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && !is_space(text[i])) {
            ++i;
        }
        if (i > start) {
            words.emplace_back(text.substr(start, i - start));
        }
    }
    return words;
}

std::string base_name(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string key_value(std::string_view text, std::string_view key) {
    for (const auto& line : split_lines(text)) {
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 &&
            line[key.size()] == '=') {
            return trim(std::string_view(line).substr(key.size() + 1));
        }
    }
    return {};
}

std::string unquote(std::string_view text) {
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
        text.back() == text.front()) {
        return std::string(text.substr(1, text.size() - 2));
    }
    return std::string(text);
}

}  // namespace radioberry::probe
