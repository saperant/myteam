// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "taskml/export.hpp"

namespace taskml {

/// Error thrown by Json::parse on malformed input.
class TASKML_API JsonParseError : public std::runtime_error {
public:
    JsonParseError(const std::string& message, std::size_t offset)
        : std::runtime_error(message), offset_(offset) {}
    /// Byte offset in the input where the error was detected.
    std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

/// A minimal JSON value. Used for free-form data in TaskML documents
/// (the `---context` block and handoff `context:` field) and for the
/// JSON interchange format. Object keys keep their insertion order.
class TASKML_API Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() noexcept = default;
    Json(std::nullptr_t) noexcept {}
    Json(bool b) : value_(b) {}
    Json(int n) : value_(static_cast<double>(n)) {}
    Json(long n) : value_(static_cast<double>(n)) {}
    Json(long long n) : value_(static_cast<double>(n)) {}
    Json(unsigned n) : value_(static_cast<double>(n)) {}
    Json(unsigned long n) : value_(static_cast<double>(n)) {}
    Json(unsigned long long n) : value_(static_cast<double>(n)) {}
    Json(double n) : value_(n) {}
    Json(const char* s) : value_(std::string(s)) {}
    Json(std::string s) : value_(std::move(s)) {}
    Json(std::string_view s) : value_(std::string(s)) {}
    Json(Array a) : value_(std::move(a)) {}
    Json(Object o) : value_(std::move(o)) {}

    static Json array(std::initializer_list<Json> items = {}) { return Json(Array(items)); }
    static Json object() { return Json(Object{}); }

    Type type() const noexcept { return static_cast<Type>(value_.index()); }
    bool is_null() const noexcept { return type() == Type::Null; }
    bool is_bool() const noexcept { return type() == Type::Bool; }
    bool is_number() const noexcept { return type() == Type::Number; }
    bool is_string() const noexcept { return type() == Type::String; }
    bool is_array() const noexcept { return type() == Type::Array; }
    bool is_object() const noexcept { return type() == Type::Object; }

    /// Accessors; throw std::bad_variant_access on type mismatch.
    bool as_bool() const { return std::get<bool>(value_); }
    double as_number() const { return std::get<double>(value_); }
    const std::string& as_string() const { return std::get<std::string>(value_); }
    std::string& as_string() { return std::get<std::string>(value_); }
    const Array& as_array() const { return std::get<Array>(value_); }
    Array& as_array() { return std::get<Array>(value_); }
    const Object& as_object() const { return std::get<Object>(value_); }
    Object& as_object() { return std::get<Object>(value_); }

    /// Object lookup. Returns nullptr when not an object or key is absent.
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);

    /// Object insert-or-assign. Converts a null value into an empty object.
    Json& set(std::string key, Json value);

    /// Array append. Converts a null value into an empty array.
    void push_back(Json value);

    /// Serialize. indent < 0 produces compact output.
    std::string dump(int indent = -1) const;

    /// Parse a JSON text. Throws JsonParseError.
    static Json parse(std::string_view text);

    friend bool operator==(const Json& a, const Json& b) { return a.value_ == b.value_; }
    friend bool operator!=(const Json& a, const Json& b) { return !(a == b); }

private:
    std::variant<std::monostate, bool, double, std::string, Array, Object> value_;
};

}  // namespace taskml
