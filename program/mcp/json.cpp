#include "json.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static const Json nullJson;

auto Json::operator[](const std::string& key) const -> const Json& {
    if (type == Type::Object) {
        for (auto& m : members)
            if (m.first == key)
                return m.second;
    }
    return nullJson;
}

auto Json::set(const std::string& key, Json value) -> Json& {
    if (type != Type::Object) {
        *this = object();
    }
    for (auto& m : members) {
        if (m.first == key) {
            m.second = std::move(value);
            return *this;
        }
    }
    members.emplace_back(key, std::move(value));
    return *this;
}

auto Json::has(const std::string& key) const -> bool {
    if (type != Type::Object)
        return false;
    for (auto& m : members)
        if (m.first == key)
            return true;
    return false;
}

auto Json::push(Json value) -> Json& {
    if (type != Type::Array)
        *this = array();
    items.push_back(std::move(value));
    return *this;
}

auto Json::getString(const std::string& key, const std::string& def) const -> std::string {
    auto& v = (*this)[key];
    return v.isString() ? v.string : def;
}

auto Json::getNumber(const std::string& key, double def) const -> double {
    auto& v = (*this)[key];
    return v.isNumber() ? v.number : def;
}

auto Json::getInt(const std::string& key, int64_t def) const -> int64_t {
    auto& v = (*this)[key];
    return v.isNumber() ? (int64_t)std::llround(v.number) : def;
}

auto Json::getBool(const std::string& key, bool def) const -> bool {
    auto& v = (*this)[key];
    return v.isBool() ? v.boolean : def;
}

// ---------------------------------------------------------------- serializer

static auto dumpString(const std::string& s, std::string& out) -> void {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else
                    out += (char)c;
        }
    }
    out += '"';
}

static auto dumpValue(const Json& j, std::string& out) -> void {
    switch (j.type) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += j.boolean ? "true" : "false"; break;
        case Json::Type::Number: {
            double n = j.number;
            if (!std::isfinite(n)) {
                out += "null";
            } else if (n == std::floor(n) && std::fabs(n) < 9.007199254740992e15) {
                out += std::to_string((int64_t)n);
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", n);
                out += buf;
            }
        } break;
        case Json::Type::String: dumpString(j.string, out); break;
        case Json::Type::Array: {
            out += '[';
            bool first = true;
            for (auto& item : j.items) {
                if (!first) out += ',';
                first = false;
                dumpValue(item, out);
            }
            out += ']';
        } break;
        case Json::Type::Object: {
            out += '{';
            bool first = true;
            for (auto& m : j.members) {
                if (!first) out += ',';
                first = false;
                dumpString(m.first, out);
                out += ':';
                dumpValue(m.second, out);
            }
            out += '}';
        } break;
    }
}

auto Json::dump() const -> std::string {
    std::string out;
    dumpValue(*this, out);
    return out;
}

// ---------------------------------------------------------------- parser

namespace {

struct Parser {
    const std::string& s;
    size_t pos = 0;
    std::string error;
    unsigned depth = 0;

    explicit Parser(const std::string& s) : s(s) {}

    auto fail(const std::string& msg) -> bool {
        if (error.empty())
            error = msg + " at offset " + std::to_string(pos);
        return false;
    }

    auto skipWs() -> void {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            pos++;
    }

    auto literal(const char* word) -> bool {
        size_t len = std::char_traits<char>::length(word);
        if (s.compare(pos, len, word) != 0)
            return false;
        pos += len;
        return true;
    }

    static auto appendUtf8(std::string& out, uint32_t cp) -> void {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xc0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += (char)(0xe0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3f));
            out += (char)(0x80 | (cp & 0x3f));
        } else {
            out += (char)(0xf0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3f));
            out += (char)(0x80 | ((cp >> 6) & 0x3f));
            out += (char)(0x80 | (cp & 0x3f));
        }
    }

    auto hex4(uint32_t& v) -> bool {
        if (pos + 4 > s.size())
            return fail("truncated \\u escape");
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = s[pos++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("invalid \\u escape");
        }
        return true;
    }

    auto parseString(std::string& out) -> bool {
        // s[pos] == '"'
        pos++;
        while (true) {
            if (pos >= s.size())
                return fail("unterminated string");
            char c = s[pos++];
            if (c == '"')
                return true;
            if ((unsigned char)c < 0x20)
                return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= s.size())
                return fail("unterminated escape");
            char e = s[pos++];
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
                    uint32_t cp;
                    if (!hex4(cp))
                        return false;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        uint32_t lo;
                        if (pos + 2 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                            pos += 2;
                            if (!hex4(lo))
                                return false;
                            if (lo >= 0xdc00 && lo <= 0xdfff)
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            else
                                cp = 0xfffd;
                        } else
                            cp = 0xfffd;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff)
                        cp = 0xfffd;
                    appendUtf8(out, cp);
                } break;
                default:
                    return fail("invalid escape");
            }
        }
    }

    auto parseNumber(Json& out) -> bool {
        size_t start = pos;
        if (s[pos] == '-') pos++;
        if (pos >= s.size() || !std::isdigit((unsigned char)s[pos]))
            return fail("invalid number");
        while (pos < s.size() && std::isdigit((unsigned char)s[pos])) pos++;
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            if (pos >= s.size() || !std::isdigit((unsigned char)s[pos]))
                return fail("invalid number");
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            if (pos >= s.size() || !std::isdigit((unsigned char)s[pos]))
                return fail("invalid number");
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) pos++;
        }
        out = Json(std::strtod(s.substr(start, pos - start).c_str(), nullptr));
        return true;
    }

    auto parseValue(Json& out) -> bool {
        if (++depth > 256)
            return fail("nesting too deep");
        skipWs();
        if (pos >= s.size())
            return fail("unexpected end");
        char c = s[pos];
        bool ok = true;
        if (c == '{') {
            pos++;
            out = Json::object();
            skipWs();
            if (pos < s.size() && s[pos] == '}') {
                pos++;
            } else {
                while (true) {
                    skipWs();
                    if (pos >= s.size() || s[pos] != '"') { ok = fail("expected key"); break; }
                    std::string key;
                    if (!parseString(key)) { ok = false; break; }
                    skipWs();
                    if (pos >= s.size() || s[pos] != ':') { ok = fail("expected ':'"); break; }
                    pos++;
                    Json value;
                    if (!parseValue(value)) { ok = false; break; }
                    out.set(key, std::move(value));
                    skipWs();
                    if (pos < s.size() && s[pos] == ',') { pos++; continue; }
                    if (pos < s.size() && s[pos] == '}') { pos++; break; }
                    ok = fail("expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            pos++;
            out = Json::array();
            skipWs();
            if (pos < s.size() && s[pos] == ']') {
                pos++;
            } else {
                while (true) {
                    Json value;
                    if (!parseValue(value)) { ok = false; break; }
                    out.items.push_back(std::move(value));
                    skipWs();
                    if (pos < s.size() && s[pos] == ',') { pos++; continue; }
                    if (pos < s.size() && s[pos] == ']') { pos++; break; }
                    ok = fail("expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            out = Json(std::string());
            ok = parseString(out.string);
        } else if (c == '-' || std::isdigit((unsigned char)c)) {
            ok = parseNumber(out);
        } else if (literal("true")) {
            out = Json(true);
        } else if (literal("false")) {
            out = Json(false);
        } else if (literal("null")) {
            out = Json();
        } else {
            ok = fail("unexpected character");
        }
        depth--;
        return ok;
    }
};

}

auto Json::parse(const std::string& text, Json& out, std::string& error) -> bool {
    Parser p(text);
    if (!p.parseValue(out)) {
        error = p.error;
        return false;
    }
    p.skipWs();
    if (p.pos != text.size()) {
        error = "trailing characters at offset " + std::to_string(p.pos);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- base64

namespace Base64 {

static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

auto encode(const uint8_t* data, size_t size) -> std::string {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (i + 1 == size) {
        uint32_t v = data[i] << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == size) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

auto decode(const std::string& text, std::vector<uint8_t>& out) -> bool {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    bool padding = false;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') { padding = true; continue; }
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else return false;
        if (padding)
            return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xff));
        }
    }
    return true;
}

}
