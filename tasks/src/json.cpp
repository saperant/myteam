// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
#include "taskml/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace taskml {

const Json* Json::find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& kv : as_object())
        if (kv.first == key) return &kv.second;
    return nullptr;
}

Json* Json::find(std::string_view key) {
    return const_cast<Json*>(static_cast<const Json&>(*this).find(key));
}

Json& Json::set(std::string key, Json value) {
    if (is_null()) value_ = Object{};
    auto& obj = as_object();
    for (auto& kv : obj) {
        if (kv.first == key) {
            kv.second = std::move(value);
            return kv.second;
        }
    }
    obj.emplace_back(std::move(key), std::move(value));
    return obj.back().second;
}

void Json::push_back(Json value) {
    if (is_null()) value_ = Array{};
    as_array().push_back(std::move(value));
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace {

void dump_string(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void dump_number(std::string& out, double d) {
    if (!std::isfinite(d)) {  // JSON has no NaN/Infinity
        out += "null";
        return;
    }
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        out += std::to_string(static_cast<long long>(d));
        return;
    }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, d);
    out.append(buf, res.ptr);
}

void newline(std::string& out, int indent, int depth) {
    if (indent < 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent * depth), ' ');
}

void dump_value(std::string& out, const Json& v, int indent, int depth) {
    switch (v.type()) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += v.as_bool() ? "true" : "false"; break;
        case Json::Type::Number: dump_number(out, v.as_number()); break;
        case Json::Type::String: dump_string(out, v.as_string()); break;
        case Json::Type::Array: {
            const auto& a = v.as_array();
            if (a.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                newline(out, indent, depth + 1);
                dump_value(out, a[i], indent, depth + 1);
            }
            newline(out, indent, depth);
            out += ']';
            break;
        }
        case Json::Type::Object: {
            const auto& o = v.as_object();
            if (o.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            for (std::size_t i = 0; i < o.size(); ++i) {
                if (i) out += ',';
                newline(out, indent, depth + 1);
                dump_string(out, o[i].first);
                out += indent < 0 ? ":" : ": ";
                dump_value(out, o[i].second, indent, depth + 1);
            }
            newline(out, indent, depth);
            out += '}';
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    Json parse_document() {
        skip_ws();
        Json v = parse_value(0);
        skip_ws();
        if (pos_ != s_.size()) fail("unexpected trailing characters");
        return v;
    }

private:
    static constexpr int kMaxDepth = 256;

    [[noreturn]] void fail(const std::string& msg) const {
        throw JsonParseError(msg + " at offset " + std::to_string(pos_), pos_);
    }

    void skip_ws() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool consume_literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    Json parse_value(int depth) {
        if (depth > kMaxDepth) fail("maximum nesting depth exceeded");
        if (pos_ >= s_.size()) fail("unexpected end of input");
        char c = s_[pos_];
        switch (c) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return Json(parse_string());
            case 't':
                if (consume_literal("true")) return Json(true);
                break;
            case 'f':
                if (consume_literal("false")) return Json(false);
                break;
            case 'n':
                if (consume_literal("null")) return Json();
                break;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        }
        fail(std::string("unexpected character '") + c + "'");
    }

    Json parse_object(int depth) {
        ++pos_;  // {
        Json::Object obj;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return Json(std::move(obj));
        }
        for (;;) {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected string key");
            std::string key = parse_string();
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') fail("expected ':'");
            ++pos_;
            skip_ws();
            Json value = parse_value(depth + 1);
            // Duplicate keys: last one wins (like JSON.parse).
            bool replaced = false;
            for (auto& kv : obj) {
                if (kv.first == key) {
                    kv.second = std::move(value);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) obj.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (pos_ >= s_.size()) fail("unterminated object");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return Json(std::move(obj));
            }
            fail("expected ',' or '}'");
        }
    }

    Json parse_array(int depth) {
        ++pos_;  // [
        Json::Array arr;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return Json(std::move(arr));
        }
        for (;;) {
            skip_ws();
            arr.push_back(parse_value(depth + 1));
            skip_ws();
            if (pos_ >= s_.size()) fail("unterminated array");
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return Json(std::move(arr));
            }
            fail("expected ',' or ']'");
        }
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    unsigned parse_hex4() {
        if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid \\u escape");
        }
        return v;
    }

    std::string parse_string() {
        ++pos_;  // "
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) fail("unterminated string");
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (s_.substr(pos_, 2) != "\\u") fail("unpaired surrogate");
                        pos_ += 2;
                        unsigned lo = parse_hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail(std::string("invalid escape '\\") + e + "'");
            }
        }
    }

    Json parse_number() {
        std::size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size()) fail("invalid number");
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            fail("invalid number");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') fail("invalid number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') fail("invalid number");
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        // strtod is locale dependent; from_chars is not.
        double d = 0;
        auto res = std::from_chars(s_.data() + start, s_.data() + pos_, d);
        if (res.ec == std::errc::result_out_of_range) {
            // Overflow -> +/-inf, underflow -> 0 (strtod semantics).
            d = std::strtod(std::string(s_.substr(start, pos_ - start)).c_str(), nullptr);
        } else if (res.ec != std::errc()) {
            fail("invalid number");
        }
        return Json(d);
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

}  // namespace

std::string Json::dump(int indent) const {
    std::string out;
    dump_value(out, *this, indent, 0);
    return out;
}

Json Json::parse(std::string_view text) {
    return JsonParser(text).parse_document();
}

}  // namespace taskml
