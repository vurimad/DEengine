#pragma once

#include <string_view>

namespace de::core {

// Helpers for the engine's line-based text files (config/input.bindings, config/engine.settings):
// one entry a line, blanks around tokens, '#' comments, Windows line ends and byte order marks
// tolerated.

constexpr bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

constexpr std::string_view trim(std::string_view text) {
    while (!text.empty() && is_blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// The text without a UTF-8 byte order mark at its start (an editor on Windows may write one).
constexpr std::string_view skip_bom(std::string_view text) {
    return text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text;
}

// The next line of text (without its '\n'), taken off the front of text.
constexpr std::string_view next_line(std::string_view& text) {
    const size_t end = text.find('\n');
    const std::string_view line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    return line;
}

// The line up to its comment ('#'), trimmed.
constexpr std::string_view strip_comment(std::string_view line) {
    return trim(line.substr(0, line.find('#')));
}

} // namespace de::core
