#pragma once

// Minimal JSON value, parser and serializer for the MCP server.
// Numbers are stored as double; strings are UTF-8.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type(Type::Bool), boolean(b) {}
    Json(int n) : type(Type::Number), number(n) {}
    Json(unsigned n) : type(Type::Number), number(n) {}
    Json(int64_t n) : type(Type::Number), number((double)n) {}
    Json(uint64_t n) : type(Type::Number), number((double)n) {}
    Json(double n) : type(Type::Number), number(n) {}
    Json(const char* s) : type(Type::String), string(s) {}
    Json(std::string s) : type(Type::String), string(std::move(s)) {}

    static auto array() -> Json { Json j; j.type = Type::Array; return j; }
    static auto object() -> Json { Json j; j.type = Type::Object; return j; }

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> items;
    // insertion order is kept for readable output
    std::vector<std::pair<std::string, Json>> members;

    auto isNull() const -> bool { return type == Type::Null; }
    auto isBool() const -> bool { return type == Type::Bool; }
    auto isNumber() const -> bool { return type == Type::Number; }
    auto isString() const -> bool { return type == Type::String; }
    auto isArray() const -> bool { return type == Type::Array; }
    auto isObject() const -> bool { return type == Type::Object; }

    // object access; returns a static null value for missing keys
    auto operator[](const std::string& key) const -> const Json&;
    // object insert/replace
    auto set(const std::string& key, Json value) -> Json&;
    auto has(const std::string& key) const -> bool;
    auto push(Json value) -> Json&;

    auto getString(const std::string& key, const std::string& def = "") const -> std::string;
    auto getNumber(const std::string& key, double def = 0.0) const -> double;
    auto getInt(const std::string& key, int64_t def = 0) const -> int64_t;
    auto getBool(const std::string& key, bool def = false) const -> bool;

    auto dump() const -> std::string;

    // returns false on syntax error, error message in 'error'
    static auto parse(const std::string& text, Json& out, std::string& error) -> bool;
};

namespace Base64 {
    auto encode(const uint8_t* data, size_t size) -> std::string;
    auto decode(const std::string& text, std::vector<uint8_t>& out) -> bool;
}
