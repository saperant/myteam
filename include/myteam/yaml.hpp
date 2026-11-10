// A small YAML subset for front matter and project configuration.
//
// Supported: block mappings and sequences (nested by indentation), "- key: v"
// sequences of mappings, flow sequences and mappings ([a, b], {a: 1}),
// plain, 'single' and "double" quoted scalars, | and > block scalars, and #
// comments. Not supported: anchors, aliases, tags, multi-document streams,
// complex keys. All scalars are kept as text; typed accessors convert.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace myteam::yaml {

class Error : public std::runtime_error {
public:
    Error(const std::string& message, int line)
        : std::runtime_error(line > 0 ? "line " + std::to_string(line) + ": " + message : message),
          line_(line) {}
    int line() const noexcept { return line_; }

private:
    int line_;
};

class Node {
public:
    enum class Kind { Null, Scalar, Sequence, Mapping };
    using Entry = std::pair<std::string, Node>;

    Node() = default;
    static Node scalar(std::string text, bool quoted = false);
    static Node sequence();
    static Node mapping();

    Kind kind() const noexcept { return kind_; }
    bool is_null() const noexcept { return kind_ == Kind::Null; }
    bool is_scalar() const noexcept { return kind_ == Kind::Scalar; }
    bool is_sequence() const noexcept { return kind_ == Kind::Sequence; }
    bool is_mapping() const noexcept { return kind_ == Kind::Mapping; }
    explicit operator bool() const noexcept { return !is_null(); }

    /// Scalar text ("" for null; throws for collections).
    const std::string& text() const;
    bool quoted() const noexcept { return quoted_; }

    /// Typed scalar access; throw Error when the text does not convert.
    std::string as_string() const { return text(); }
    long long as_int() const;
    double as_double() const;
    bool as_bool() const;
    /// A scalar as a one-element list, a sequence of scalars as a list.
    std::vector<std::string> as_strings() const;

    // Collections.
    const std::vector<Node>& items() const;      // sequence
    const std::vector<Entry>& entries() const;   // mapping
    std::vector<Node>& items();
    std::vector<Entry>& entries();
    std::size_t size() const noexcept;

    /// Mapping lookup; a null node when absent or not a mapping.
    const Node& operator[](std::string_view key) const;
    bool has(std::string_view key) const;

    std::string get(std::string_view key, std::string fallback = {}) const;
    long long get_int(std::string_view key, long long fallback) const;
    double get_double(std::string_view key, double fallback) const;
    bool get_bool(std::string_view key, bool fallback) const;

    void push_back(Node n);
    void set(std::string key, Node n);

    int line = 0;  ///< Source line of the node (1-based), 0 when built.

private:
    Kind kind_ = Kind::Null;
    bool quoted_ = false;
    std::string text_;
    std::vector<Node> items_;
    std::vector<Entry> entries_;
};

/// Parse a YAML document. `first_line` numbers the first line of `text`
/// (for front matter that starts below a fence). Throws Error.
Node parse(std::string_view text, int first_line = 1);

}  // namespace myteam::yaml
