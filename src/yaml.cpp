#include "myteam/yaml.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>

namespace myteam::yaml {

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

Node Node::scalar(std::string text, bool quoted) {
    Node n;
    n.kind_ = Kind::Scalar;
    n.text_ = std::move(text);
    n.quoted_ = quoted;
    return n;
}

Node Node::sequence() {
    Node n;
    n.kind_ = Kind::Sequence;
    return n;
}

Node Node::mapping() {
    Node n;
    n.kind_ = Kind::Mapping;
    return n;
}

const std::string& Node::text() const {
    static const std::string empty;
    if (kind_ == Kind::Null) return empty;
    if (kind_ != Kind::Scalar) throw Error("expected a scalar value", line);
    return text_;
}

long long Node::as_int() const {
    const std::string& t = text();
    long long v = 0;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc() || p != t.data() + t.size()) throw Error("expected an integer, got \"" + t + "\"", line);
    return v;
}

double Node::as_double() const {
    const std::string& t = text();
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (t.empty() || end != t.c_str() + t.size()) throw Error("expected a number, got \"" + t + "\"", line);
    return v;
}

bool Node::as_bool() const {
    const std::string& t = text();
    if (t == "true" || t == "True" || t == "yes" || t == "on") return true;
    if (t == "false" || t == "False" || t == "no" || t == "off") return false;
    throw Error("expected true or false, got \"" + t + "\"", line);
}

std::vector<std::string> Node::as_strings() const {
    std::vector<std::string> out;
    if (is_null()) return out;
    if (is_scalar()) {
        out.push_back(text_);
        return out;
    }
    for (const Node& n : items()) out.push_back(n.text());
    return out;
}

const std::vector<Node>& Node::items() const {
    static const std::vector<Node> empty;
    if (kind_ == Kind::Null) return empty;
    if (kind_ != Kind::Sequence) throw Error("expected a list", line);
    return items_;
}

const std::vector<Node::Entry>& Node::entries() const {
    static const std::vector<Entry> empty;
    if (kind_ == Kind::Null) return empty;
    if (kind_ != Kind::Mapping) throw Error("expected a mapping", line);
    return entries_;
}

std::vector<Node>& Node::items() {
    if (kind_ != Kind::Sequence) throw Error("expected a list", line);
    return items_;
}

std::vector<Node::Entry>& Node::entries() {
    if (kind_ != Kind::Mapping) throw Error("expected a mapping", line);
    return entries_;
}

std::size_t Node::size() const noexcept {
    switch (kind_) {
        case Kind::Sequence: return items_.size();
        case Kind::Mapping: return entries_.size();
        default: return 0;
    }
}

const Node& Node::operator[](std::string_view key) const {
    static const Node null;
    if (kind_ != Kind::Mapping) return null;
    for (const auto& [k, v] : entries_)
        if (k == key) return v;
    return null;
}

bool Node::has(std::string_view key) const {
    if (kind_ != Kind::Mapping) return false;
    for (const auto& e : entries_)
        if (e.first == key) return true;
    return false;
}

std::string Node::get(std::string_view key, std::string fallback) const {
    const Node& n = (*this)[key];
    return n.is_null() ? fallback : n.text();
}

long long Node::get_int(std::string_view key, long long fallback) const {
    const Node& n = (*this)[key];
    return n.is_null() ? fallback : n.as_int();
}

double Node::get_double(std::string_view key, double fallback) const {
    const Node& n = (*this)[key];
    return n.is_null() ? fallback : n.as_double();
}

bool Node::get_bool(std::string_view key, bool fallback) const {
    const Node& n = (*this)[key];
    return n.is_null() ? fallback : n.as_bool();
}

void Node::push_back(Node n) {
    if (kind_ == Kind::Null) kind_ = Kind::Sequence;
    items().push_back(std::move(n));
}

void Node::set(std::string key, Node n) {
    if (kind_ == Kind::Null) kind_ = Kind::Mapping;
    for (auto& e : entries())
        if (e.first == key) {
            e.second = std::move(n);
            return;
        }
    entries_.emplace_back(std::move(key), std::move(n));
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

struct Line {
    int number = 0;
    int indent = 0;
    std::string content;  // without indentation and trailing whitespace
    std::string raw;      // the full line (for block scalars)
    bool blank = false;   // empty or comment only
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Remove a trailing "# comment" that is outside quotes.
std::string strip_comment(std::string_view s) {
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"') ++i;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            if (i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '[' ||
                s[i - 1] == '{' || s[i - 1] == ',' || s[i - 1] == ':')
                quote = c;
        } else if (c == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1])))) {
            return std::string(trim(s.substr(0, i)));
        }
    }
    return std::string(trim(s));
}

// Position of the ':' that ends a mapping key (followed by space or end),
// outside quotes and brackets; npos when the line is not "key: ...".
std::size_t key_colon(std::string_view s) {
    if (s.empty() || s[0] == '[' || s[0] == '{' || s[0] == '#') return std::string_view::npos;
    char quote = 0;
    std::size_t start = 0;
    if (s[0] == '"' || s[0] == '\'') {
        quote = s[0];
        start = 1;
        for (std::size_t i = 1; i < s.size(); ++i) {
            if (s[i] == '\\' && quote == '"') {
                ++i;
                continue;
            }
            if (s[i] == quote) {
                start = i + 1;
                quote = 0;
                break;
            }
        }
        if (quote) return std::string_view::npos;
    }
    for (std::size_t i = start; i < s.size(); ++i) {
        if (s[i] == ':' && (i + 1 == s.size() || s[i + 1] == ' ' || s[i + 1] == '\t')) return i;
        if (s[i] == ' ' && i + 1 < s.size() && s[i + 1] == '#') return std::string_view::npos;
    }
    return std::string_view::npos;
}

bool is_seq_item(std::string_view c) { return c == "-" || (c.size() >= 2 && c[0] == '-' && c[1] == ' '); }

std::string unquote_double(std::string_view s, int line) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= s.size()) throw Error("unterminated escape in string", line);
        switch (s[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case ' ': out += ' '; break;
            default: out += '\\'; out += s[i]; break;
        }
    }
    return out;
}

std::string unquote_single(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        out += s[i];
        if (s[i] == '\'' && i + 1 < s.size() && s[i + 1] == '\'') ++i;
    }
    return out;
}

Node make_scalar(std::string_view s, int line) {
    s = trim(s);
    Node n;
    if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") {
        n.line = line;
        return n;
    }
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        n = Node::scalar(unquote_double(s.substr(1, s.size() - 2), line), true);
    else if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
        n = Node::scalar(unquote_single(s.substr(1, s.size() - 2)), true);
    else if (s.front() == '"' || s.front() == '\'')
        throw Error("unterminated quoted string", line);
    else
        n = Node::scalar(std::string(s));
    n.line = line;
    return n;
}

// Flow collections: [a, b], {k: v}, nested.
class Flow {
public:
    Flow(std::string_view s, int line) : s_(s), line_(line) {}

    Node parse() {
        Node n = value();
        skip_ws();
        if (i_ != s_.size()) fail("unexpected text after value");
        return n;
    }

private:
    [[noreturn]] void fail(const std::string& what) { throw Error(what + " in \"" + std::string(s_) + "\"", line_); }

    void skip_ws() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }

    Node value() {
        skip_ws();
        if (i_ >= s_.size()) fail("expected a value");
        if (s_[i_] == '[') return seq();
        if (s_[i_] == '{') return map();
        return scalar();
    }

    Node seq() {
        Node n = Node::sequence();
        n.line = line_;
        ++i_;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return n;
        }
        while (true) {
            n.push_back(value());
            skip_ws();
            if (i_ >= s_.size()) fail("unterminated [");
            if (s_[i_] == ',') {
                ++i_;
                skip_ws();
                if (i_ < s_.size() && s_[i_] == ']') {  // trailing comma
                    ++i_;
                    return n;
                }
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return n;
            }
            fail("expected , or ]");
        }
    }

    Node map() {
        Node n = Node::mapping();
        n.line = line_;
        ++i_;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return n;
        }
        while (true) {
            Node key = scalar(/*key=*/true);
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') fail("expected : after key");
            ++i_;
            skip_ws();
            Node v;
            if (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}') v = value();
            n.set(key.text(), std::move(v));
            skip_ws();
            if (i_ >= s_.size()) fail("unterminated {");
            if (s_[i_] == ',') {
                ++i_;
                skip_ws();
                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    return n;
                }
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return n;
            }
            fail("expected , or }");
        }
    }

    Node scalar(bool key = false) {
        skip_ws();
        if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '\'')) {
            char q = s_[i_];
            std::size_t start = i_++;
            while (i_ < s_.size()) {
                if (q == '"' && s_[i_] == '\\') {
                    i_ += 2;
                    continue;
                }
                if (s_[i_] == q) {
                    if (q == '\'' && i_ + 1 < s_.size() && s_[i_ + 1] == '\'') {
                        i_ += 2;
                        continue;
                    }
                    break;
                }
                ++i_;
            }
            if (i_ >= s_.size()) fail("unterminated string");
            ++i_;
            return make_scalar(s_.substr(start, i_ - start), line_);
        }
        std::size_t start = i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ',' || c == ']' || c == '}') break;
            if (key && c == ':' && (i_ + 1 >= s_.size() || s_[i_ + 1] == ' ' || s_[i_ + 1] == ',' ||
                                    s_[i_ + 1] == '}'))
                break;
            ++i_;
        }
        return make_scalar(s_.substr(start, i_ - start), line_);
    }

    std::string_view s_;
    int line_;
    std::size_t i_ = 0;
};

class Parser {
public:
    Parser(std::string_view text, int first_line) {
        std::size_t start = 0;
        int number = first_line;
        while (start <= text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            std::string_view raw = text.substr(start, end - start);
            if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
            Line l;
            l.number = number++;
            l.raw = std::string(raw);
            std::size_t i = 0;
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
            l.indent = static_cast<int>(i);
            l.content = std::string(trim(raw.substr(i)));
            l.blank = l.content.empty() || l.content[0] == '#';
            lines_.push_back(std::move(l));
            if (end == text.size()) break;
            start = end + 1;
        }
    }

    Node run() {
        skip_blank();
        if (pos_ >= lines_.size()) return Node();
        Node n = block(lines_[pos_].indent);
        skip_blank();
        if (pos_ < lines_.size())
            throw Error("unexpected indentation or content", lines_[pos_].number);
        return n;
    }

private:
    void skip_blank() {
        while (pos_ < lines_.size() && lines_[pos_].blank) ++pos_;
    }

    // A node whose lines start at `indent`.
    Node block(int indent) {
        skip_blank();
        const Line& l = lines_[pos_];
        if (is_seq_item(l.content)) return sequence(indent);
        if (key_colon(l.content) != std::string_view::npos) return mapping(indent);
        // A plain or flow scalar, possibly over several lines.
        int line = l.number;
        std::string text = strip_comment(l.content);
        ++pos_;
        while (true) {
            std::size_t save = pos_;
            skip_blank();
            if (pos_ >= lines_.size() || lines_[pos_].indent < indent || is_seq_item(lines_[pos_].content) ||
                key_colon(lines_[pos_].content) != std::string_view::npos) {
                pos_ = save;
                break;
            }
            text += ' ' + strip_comment(lines_[pos_].content);
            ++pos_;
        }
        return inline_value(text, line);
    }

    Node inline_value(const std::string& text, int line) {
        if (!text.empty() && (text[0] == '[' || text[0] == '{')) return Flow(text, line).parse();
        return make_scalar(text, line);
    }

    Node mapping(int indent) {
        Node map = Node::mapping();
        map.line = lines_[pos_].number;
        while (true) {
            skip_blank();
            if (pos_ >= lines_.size()) break;
            Line& l = lines_[pos_];
            if (l.indent < indent) break;
            if (l.indent > indent) throw Error("unexpected indentation", l.number);
            if (is_seq_item(l.content)) break;
            std::size_t colon = key_colon(l.content);
            if (colon == std::string_view::npos) throw Error("expected key: value", l.number);
            Node key = make_scalar(std::string_view(l.content).substr(0, colon), l.number);
            std::string k = key.text();
            if (map.has(k)) throw Error("duplicate key \"" + k + "\"", l.number);
            std::string rest = strip_comment(std::string_view(l.content).substr(colon + 1));
            const int line = l.number;
            ++pos_;
            map.set(std::move(k), value_after_key(rest, indent, line));
        }
        return map;
    }

    // The value of "key: <rest>" (or "- <rest>") whose key is at `indent`.
    Node value_after_key(const std::string& rest, int indent, int line) {
        if (!rest.empty() && (rest[0] == '|' || rest[0] == '>')) return block_scalar(rest, indent, line);
        if (!rest.empty()) {
            if ((rest[0] == '[' || rest[0] == '{') && !balanced(rest)) return multiline_flow(rest, line);
            return inline_value(rest, line);
        }
        // Nested block below, or a sequence at the same indentation.
        std::size_t save = pos_;
        skip_blank();
        if (pos_ < lines_.size()) {
            const Line& next = lines_[pos_];
            if (next.indent > indent) return block(next.indent);
            if (next.indent == indent && is_seq_item(next.content)) return sequence(indent);
        }
        pos_ = save;
        Node n;
        n.line = line;
        return n;
    }

    static bool balanced(const std::string& s) {
        int depth = 0;
        char quote = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (quote) {
                if (c == '\\' && quote == '"') ++i;
                else if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') quote = c;
            else if (c == '[' || c == '{') ++depth;
            else if (c == ']' || c == '}') --depth;
        }
        return depth <= 0;
    }

    Node multiline_flow(std::string text, int line) {
        while (!balanced(text)) {
            if (pos_ >= lines_.size()) throw Error("unterminated flow collection", line);
            const Line& l = lines_[pos_++];
            if (!l.blank) text += ' ' + strip_comment(l.content);
        }
        return Flow(text, line).parse();
    }

    Node block_scalar(const std::string& header, int indent, int line) {
        const bool literal = header[0] == '|';
        char chomp = 0;  // '-' strip, '+' keep, 0 clip
        for (char c : header.substr(1))
            if (c == '-' || c == '+') chomp = c;
        std::vector<std::string> body;
        int block_indent = -1;
        while (pos_ < lines_.size()) {
            const Line& l = lines_[pos_];
            bool empty = trim(l.raw).empty();
            if (!empty) {
                if (l.indent <= indent) break;
                if (block_indent < 0) block_indent = l.indent;
                if (l.indent < block_indent) break;
            }
            body.push_back(empty ? std::string() : l.raw.substr(static_cast<std::size_t>(block_indent)));
            ++pos_;
        }
        // Trailing empty lines belong to the chomping, not to the next key.
        std::size_t trailing = 0;
        while (trailing < body.size() && body[body.size() - 1 - trailing].empty()) ++trailing;
        std::string out;
        const std::size_t n = body.size() - trailing;
        for (std::size_t i = 0; i < n; ++i) {
            if (i > 0) {
                if (literal) {
                    out += '\n';
                } else if (body[i - 1].empty() && !body[i].empty() && body[i][0] != ' ') {
                    // The blank line before already gave the line break.
                } else if (!body[i].empty() && !body[i - 1].empty() && body[i][0] != ' ' && body[i - 1][0] != ' ') {
                    out += ' ';
                } else {
                    out += '\n';
                }
            }
            out += body[i];
        }
        if (n > 0 && chomp != '-') out += '\n';
        if (chomp == '+') out.append(trailing, '\n');
        Node s = Node::scalar(std::move(out), true);
        s.line = line;
        return s;
    }

    Node sequence(int indent) {
        Node seq = Node::sequence();
        seq.line = lines_[pos_].number;
        while (true) {
            skip_blank();
            if (pos_ >= lines_.size()) break;
            Line& l = lines_[pos_];
            if (l.indent < indent) break;
            if (l.indent > indent) throw Error("unexpected indentation", l.number);
            if (!is_seq_item(l.content)) break;
            std::string_view rest = trim(std::string_view(l.content).substr(1));
            if (rest.empty()) {
                const int line = l.number;
                ++pos_;
                seq.push_back(value_after_key("", indent, line));
                continue;
            }
            // "- key: v" or "- - x": the item is a block starting at the
            // column after the dash. Rewrite the line in place and recurse.
            std::size_t offset = l.content.size() - rest.size();
            if (is_seq_item(rest) || key_colon(rest) != std::string_view::npos) {
                l.indent += static_cast<int>(offset);
                l.content = std::string(rest);
                seq.push_back(block(l.indent));
                continue;
            }
            const int line = l.number;
            std::string text = strip_comment(rest);
            ++pos_;
            if (!text.empty() && (text[0] == '|' || text[0] == '>'))
                seq.push_back(block_scalar(text, indent, line));
            else if (!text.empty() && (text[0] == '[' || text[0] == '{') && !balanced(text))
                seq.push_back(multiline_flow(text, line));
            else
                seq.push_back(inline_value(text, line));
        }
        return seq;
    }

    std::vector<Line> lines_;
    std::size_t pos_ = 0;
};

}  // namespace

Node parse(std::string_view text, int first_line) { return Parser(text, first_line).run(); }

}  // namespace myteam::yaml
