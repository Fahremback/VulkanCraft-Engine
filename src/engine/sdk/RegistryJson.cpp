#include "RegistryJson.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>

namespace engine {
namespace sdk {

namespace {

bool is_canonical_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    const int groups[5] = { 8, 4, 4, 4, 12 };
    std::size_t cursor = 0;
    for (int g = 0; g < 5; ++g) {
        for (int i = 0; i < groups[g]; ++i) {
            const char c = value[cursor++];
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                             (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
        if (g < 4 && value[cursor++] != '-') return false;
    }
    return true;
}

std::uint64_t fnv1a(const std::string& text, std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

bool json_parse(const std::string& text, JsonValue& out, std::string& errorOut) {
    // SDK convention: clear the diagnostic buffer at entry so a stale error
    // from a previous failed call can never poison a caller that reuses the
    // buffer and checks `!errorOut.empty()` after a successful parse
    // (false-negative; see findings #152 — same bug class as #140/#142/#145).
    errorOut.clear();
    std::size_t pos = 0;
    const std::size_t length = text.size();
    int line = 1;
    int column = 1;

    const auto fail = [&](const std::string& reason) {
        std::ostringstream message;
        message << "JSON error at " << line << ':' << column << ": " << reason;
        errorOut = message.str();
        return false;
    };

    const auto skip_ws = [&]() {
        while (pos < length) {
            const char c = text[pos];
            if (c == '\n') { ++line; column = 1; ++pos; continue; }
            if (c == ' ' || c == '\t' || c == '\r') { ++column; ++pos; continue; }
            break;
        }
    };

    const auto parse_string_value = [&](std::string& outString) -> bool {
        // Assumes text[pos] == '"'.
        ++pos; ++column;
        outString.clear();
        while (pos < length) {
            const char c = text[pos];
            if (c == '"') { ++pos; ++column; return true; }
            if (c == '\\') {
                ++pos; ++column;
                if (pos >= length) return fail("unterminated escape");
                const char e = text[pos];
                switch (e) {
                case '"': outString += '"'; break;
                case '\\': outString += '\\'; break;
                case '/': outString += '/'; break;
                case 'b': outString += '\b'; break;
                case 'f': outString += '\f'; break;
                case 'n': outString += '\n'; break;
                case 'r': outString += '\r'; break;
                case 't': outString += '\t'; break;
                case 'u': {
                    ++pos; ++column;
                    if (pos + 4 > length) return fail("invalid \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text[pos + static_cast<std::size_t>(i)];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return fail("invalid \\u hex digit");
                    }
                    pos += 4; column += 4;
                    if (code < 0x80) outString += static_cast<char>(code);
                    else if (code < 0x800) {
                        outString += static_cast<char>(0xC0 | (code >> 6));
                        outString += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        outString += static_cast<char>(0xE0 | (code >> 12));
                        outString += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        outString += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    continue;
                }
                default: return fail("unknown escape");
                }
                ++pos; ++column;
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            outString += c;
            ++pos; ++column;
        }
        return fail("unterminated string");
    };

    const auto parse_number_value = [&](JsonValue& value) -> bool {
        const std::size_t start = pos;
        if (pos < length && (text[pos] == '-' || text[pos] == '+')) { ++pos; ++column; }
        bool digits = false;
        while (pos < length && std::isdigit(static_cast<unsigned char>(text[pos]))) {
            ++pos; ++column; digits = true;
        }
        if (pos < length && text[pos] == '.') {
            ++pos; ++column;
            while (pos < length && std::isdigit(static_cast<unsigned char>(text[pos]))) {
                ++pos; ++column; digits = true;
            }
        }
        if (pos < length && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos; ++column;
            if (pos < length && (text[pos] == '-' || text[pos] == '+')) { ++pos; ++column; }
            while (pos < length && std::isdigit(static_cast<unsigned char>(text[pos]))) {
                ++pos; ++column; digits = true;
            }
        }
        if (!digits) return fail("invalid number");
        const std::string token = text.substr(start, pos - start);
        value.kind = JsonValue::Kind::Number;
        value.number = std::strtod(token.c_str(), nullptr);
        return true;
    };

    std::function<bool(JsonValue&)> parse_value;
    parse_value = [&](JsonValue& value) -> bool {
        skip_ws();
        if (pos >= length) return fail("unexpected end of input");
        const char c = text[pos];
        if (c == '{') {
            ++pos; ++column;
            value.kind = JsonValue::Kind::Object;
            skip_ws();
            if (pos < length && text[pos] == '}') { ++pos; ++column; return true; }
            while (true) {
                skip_ws();
                if (pos >= length) return fail("unterminated object");
                if (text[pos] != '"') return fail("expected object key string");
                std::string key;
                if (!parse_string_value(key)) return false;
                skip_ws();
                if (pos >= length || text[pos] != ':') return fail("expected ':' after object key");
                ++pos; ++column;
                JsonValue child;
                if (!parse_value(child)) return false;
                value.object[key] = std::move(child);
                skip_ws();
                if (pos >= length) return fail("unterminated object");
                if (text[pos] == ',') { ++pos; ++column; continue; }
                if (text[pos] == '}') { ++pos; ++column; return true; }
                return fail("expected ',' or '}' in object");
            }
        }
        if (c == '[') {
            ++pos; ++column;
            value.kind = JsonValue::Kind::Array;
            skip_ws();
            if (pos < length && text[pos] == ']') { ++pos; ++column; return true; }
            while (true) {
                JsonValue child;
                if (!parse_value(child)) return false;
                value.array.push_back(std::move(child));
                skip_ws();
                if (pos >= length) return fail("unterminated array");
                if (text[pos] == ',') { ++pos; ++column; continue; }
                if (text[pos] == ']') { ++pos; ++column; return true; }
                return fail("expected ',' or ']' in array");
            }
        }
        if (c == '"') {
            std::string str;
            if (!parse_string_value(str)) return false;
            value.kind = JsonValue::Kind::String;
            value.string = std::move(str);
            return true;
        }
        if (c == 't' && text.compare(pos, 4, "true") == 0) {
            pos += 4; column += 4;
            value.kind = JsonValue::Kind::Bool;
            value.boolean = true;
            return true;
        }
        if (c == 'f' && text.compare(pos, 5, "false") == 0) {
            pos += 5; column += 5;
            value.kind = JsonValue::Kind::Bool;
            value.boolean = false;
            return true;
        }
        if (c == 'n' && text.compare(pos, 4, "null") == 0) {
            pos += 4; column += 4;
            value.kind = JsonValue::Kind::Null;
            return true;
        }
        if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number_value(value);
        }
        return fail("unexpected character");
    };

    if (!parse_value(out)) return false;
    skip_ws();
    if (pos != length) return fail("trailing content after JSON document");
    return true;
}

std::string json_string(const JsonValue& object, const std::string& key,
                        const std::string& defaultValue) {
    const JsonValue* value = object.field(key);
    if (!value || value->kind != JsonValue::Kind::String) return defaultValue;
    return value->string;
}

double json_number(const JsonValue& object, const std::string& key, double defaultValue) {
    const JsonValue* value = object.field(key);
    if (!value || value->kind != JsonValue::Kind::Number) return defaultValue;
    return value->number;
}

bool json_bool(const JsonValue& object, const std::string& key, bool defaultValue) {
    const JsonValue* value = object.field(key);
    if (!value || value->kind != JsonValue::Kind::Bool) return defaultValue;
    return value->boolean;
}

std::vector<std::string> json_string_array(const JsonValue& object, const std::string& key) {
    std::vector<std::string> result;
    const JsonValue* value = object.field(key);
    if (!value || value->kind != JsonValue::Kind::Array) return result;
    for (const JsonValue& entry : value->array) {
        if (entry.kind == JsonValue::Kind::String) result.push_back(entry.string);
    }
    return result;
}

std::vector<double> json_number_array(const JsonValue& object, const std::string& key) {
    std::vector<double> result;
    const JsonValue* value = object.field(key);
    if (!value || value->kind != JsonValue::Kind::Array) return result;
    for (const JsonValue& entry : value->array) {
        if (entry.kind == JsonValue::Kind::Number) result.push_back(entry.number);
    }
    return result;
}

std::string stable_uuid(const std::string& namespacedName) {
    const std::uint64_t high = fnv1a(namespacedName, 1469598103934665603ull);
    const std::uint64_t low = fnv1a(namespacedName, 1099511628211ull);

    char buffer[37];
    std::snprintf(buffer, sizeof(buffer),
                  "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((high >> 32) & 0xFFFFFFFFu),
                  static_cast<unsigned>((high >> 16) & 0xFFFFu),
                  static_cast<unsigned>((high >> 4) & 0xFFFFu),  // version-ish nibble
                  static_cast<unsigned>(((high & 0xF) << 12) | ((low >> 52) & 0xFFFu)),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFull));
    return std::string(buffer);
}

std::string uuid_or_derived(const std::string& uuid, const std::string& namespacedName) {
    if (is_canonical_uuid(uuid)) return uuid;
    return stable_uuid(namespacedName);
}

}  // namespace sdk
}  // namespace engine
