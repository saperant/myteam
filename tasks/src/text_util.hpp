// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
//
// Internal text helpers shared by the parser and serializer.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace taskml::detail {

// UTF-8 encodings of the verification markers.
inline constexpr std::string_view kCheck = "\xE2\x9C\x93";        // ✓ U+2713
inline constexpr std::string_view kHeavyCheck = "\xE2\x9C\x94";   // ✔ U+2714
inline constexpr std::string_view kBallotX = "\xE2\x9C\x97";      // ✗ U+2717
inline constexpr std::string_view kHeavyBallotX = "\xE2\x9C\x98"; // ✘ U+2718
inline constexpr std::string_view kCircle = "\xE2\x97\x8B";       // ○ U+25CB

inline bool is_space(char c) { return c == ' ' || c == '\t'; }

/// Identifier characters used by tags, assignees, ids, directive and view
/// names: [A-Za-z0-9_-].
inline bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline std::string_view trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

/// Length of the escapable sequence starting at s[0], or 0 if s does not
/// start with a character that may follow a backslash.
/// Escapable: # @ ~ ! ^ [ ( \ ✓ ✗ ○
inline std::size_t escapable_len(std::string_view s) {
    if (s.empty()) return 0;
    switch (s[0]) {
        case '#': case '@': case '~': case '!': case '^': case '[': case '(': case '\\':
            return 1;
        default:
            break;
    }
    if (starts_with(s, kCheck) || starts_with(s, kBallotX) || starts_with(s, kCircle)) return 3;
    return 0;
}

/// Remove backslash escapes: "\#" -> "#". A backslash not followed by an
/// escapable character is kept literally.
inline std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') {
            std::size_t n = escapable_len(s.substr(i + 1));
            if (n) {
                out.append(s.substr(i + 1, n));
                i += n;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

/// Escape backslashes that would otherwise be read as escape sequences.
/// When `tokens` is true, also escape inline-token sigils (# @ ~ ! ^) that
/// start a word, so they are not parsed as metadata tokens.
inline std::string escape(std::string_view s, bool tokens) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && escapable_len(s.substr(i + 1))) {
            out += "\\\\";
            continue;
        }
        if (tokens && (c == '#' || c == '@' || c == '~' || c == '!' || c == '^') &&
            (i == 0 || is_space(s[i - 1]))) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

/// Strip one pair of matching surrounding quotes ("..." or '...').
inline std::string_view unquote(std::string_view s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

}  // namespace taskml::detail
