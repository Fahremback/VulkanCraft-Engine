#pragma once

// JsonMini — a tiny, self-contained JSON DOM with a recursive-descent parser
// and a writer. Header-only, no external dependencies. Used by the visual
// authoring models (VisualScriptCanvas, AudioEditorModel) for file
// serialization, mirroring the hand-rolled JSON style already used by
// Serializer.cpp. Numbers are stored as doubles; UUIDs and large integer ids
// are serialized as strings to avoid precision loss.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace Engine::Json {

class Value {
public:
    enum class Kind : uint8_t { Null, Boolean, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : kind_(Kind::Boolean), bool_(v) {}
    Value(int v) : kind_(Kind::Number), number_(static_cast<double>(v)) {}
    Value(int64_t v) : kind_(Kind::Number), number_(static_cast<double>(v)) {}
    Value(double v) : kind_(Kind::Number), number_(v) {}
    Value(const char* v) : kind_(Kind::String), string_(v) {}
    Value(std::string v) : kind_(Kind::String), string_(std::move(v)) {}

    static Value make_array() { Value v; v.kind_ = Kind::Array; return v; }
    static Value make_object() { Value v; v.kind_ = Kind::Object; return v; }

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return kind_ == Kind::Boolean; }
    [[nodiscard]] bool is_number() const noexcept { return kind_ == Kind::Number; }
    [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::String; }
    [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::Array; }
    [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::Object; }

    [[nodiscard]] bool as_bool(bool def = false) const noexcept {
        return kind_ == Kind::Boolean ? bool_ : def;
    }
    [[nodiscard]] double as_number(double def = 0.0) const noexcept {
        return kind_ == Kind::Number ? number_ : def;
    }
    [[nodiscard]] int64_t as_int(int64_t def = 0) const noexcept {
        return kind_ == Kind::Number ? static_cast<int64_t>(number_) : def;
    }
    [[nodiscard]] std::string as_string(const std::string& def = {}) const {
        return kind_ == Kind::String ? string_ : def;
    }

    // --- arrays ---
    void push(Value v) {
        kind_ = Kind::Array;
        array_.push_back(std::move(v));
    }
    [[nodiscard]] const std::vector<Value>& array() const noexcept { return array_; }
    [[nodiscard]] std::vector<Value>& array() noexcept { return array_; }
    [[nodiscard]] const Value* at(std::size_t index) const noexcept {
        return index < array_.size() ? &array_[index] : nullptr;
    }

    // --- objects (stored as ordered key/value pairs; small documents) ---
    Value& operator[](std::string key) {
        if (kind_ != Kind::Object) {
            kind_ = Kind::Object;
            object_.clear();
        }
        for (auto& kv : object_) {
            if (kv.first == key) return kv.second;
        }
        object_.emplace_back(std::move(key), Value());
        return object_.back().second;
    }
    [[nodiscard]] const Value* find(const std::string& key) const noexcept {
        if (kind_ != Kind::Object) return nullptr;
        for (const auto& kv : object_) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& object() const noexcept {
        return object_;
    }
    [[nodiscard]] std::vector<std::pair<std::string, Value>>& object() noexcept {
        return object_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        switch (kind_) {
        case Kind::Array: return array_.size();
        case Kind::Object: return object_.size();
        default: return 0;
        }
    }

private:
    Kind kind_{Kind::Null};
    bool bool_{false};
    double number_{0.0};
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

// Parses a JSON document. On failure returns a null Value and, if `error` is
// non-null, writes a human-readable message into it.
Value parse(const std::string& text, std::string* error = nullptr);

// Serializes a Value to JSON. `indent >= 0` produces pretty-printed output
// with that many spaces per level; `indent < 0` produces compact output.
std::string stringify(const Value& v, int indent = -1);

namespace detail {

[[nodiscard]] inline std::string number_to_string(double value) {
    if (value == 0.0) return "0";
    if (value == static_cast<double>(static_cast<int64_t>(value)) &&
        value > -9.0e15 && value < 9.0e15) {
        return std::to_string(static_cast<int64_t>(value));
    }
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

[[nodiscard]] inline std::string escape_string(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char c : value) {
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
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

[[nodiscard]] inline bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline void append_utf8(std::string& out, uint32_t codePoint) {
    if (codePoint < 0x80u) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800u) {
        out += static_cast<char>(0xC0u | (codePoint >> 6u));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    } else if (codePoint < 0x10000u) {
        out += static_cast<char>(0xE0u | (codePoint >> 12u));
        out += static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (codePoint >> 18u));
        out += static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3Fu));
        out += static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    }
}

inline void stringify_into(const Value& v, int indent, int depth, std::string& out) {
    const std::string newline = (indent > 0) ? "\n" : "";
    std::string pad;
    if (indent > 0) pad.assign(static_cast<std::size_t>(depth * indent), ' ');
    std::string childPad;
    if (indent > 0) childPad.assign(static_cast<std::size_t>((depth + 1) * indent), ' ');

    switch (v.kind()) {
    case Value::Kind::Null:
        out += "null";
        break;
    case Value::Kind::Boolean:
        out += v.as_bool() ? "true" : "false";
        break;
    case Value::Kind::Number:
        out += number_to_string(v.as_number());
        break;
    case Value::Kind::String:
        out += '"';
        out += escape_string(v.as_string());
        out += '"';
        break;
    case Value::Kind::Array: {
        out += '[';
        const auto& items = v.array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i > 0) out += ',';
            out += newline;
            out += childPad;
            stringify_into(items[i], indent, depth + 1, out);
        }
        if (!items.empty()) {
            out += newline;
            out += pad;
        }
        out += ']';
        break;
    }
    case Value::Kind::Object: {
        out += '{';
        const auto& pairs = v.object();
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            if (i > 0) out += ',';
            out += newline;
            out += childPad;
            out += '"';
            out += escape_string(pairs[i].first);
            out += "\": ";
            stringify_into(pairs[i].second, indent, depth + 1, out);
        }
        if (!pairs.empty()) {
            out += newline;
            out += pad;
        }
        out += '}';
        break;
    }
    }
}

struct Parser {
    const std::string& text;
    std::size_t pos{0};
    std::string error;

    explicit Parser(const std::string& source) : text(source) {}

    [[nodiscard]] bool eof() const noexcept { return pos >= text.size(); }
    [[nodiscard]] char peek() const noexcept { return eof() ? '\0' : text[pos]; }

    void skip_whitespace() {
        while (!eof() && is_space(text[pos])) ++pos;
    }

    bool fail(const std::string& message) {
        if (error.empty()) {
            error = message + " at offset " + std::to_string(pos);
        }
        return false;
    }

    bool consume(char expected) {
        if (peek() == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    bool match_literal(const char* literal) {
        const std::size_t length = std::char_traits<char>::length(literal);
        if (text.compare(pos, length, literal) != 0) return false;
        pos += length;
        return true;
    }

    std::string parse_string() {
        std::string out;
        ++pos; // opening quote
        while (!eof()) {
            const char c = text[pos++];
            if (c == '"') break;
            if (c == '\\') {
                if (eof()) break;
                const char esc = text[pos++];
                switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t codeUnit = 0;
                    bool valid = true;
                    for (int i = 0; i < 4; ++i) {
                        if (eof()) { valid = false; break; }
                        const char hex = text[pos++];
                        codeUnit <<= 4u;
                        if (hex >= '0' && hex <= '9') codeUnit |= static_cast<uint32_t>(hex - '0');
                        else if (hex >= 'a' && hex <= 'f') codeUnit |= static_cast<uint32_t>(hex - 'a' + 10);
                        else if (hex >= 'A' && hex <= 'F') codeUnit |= static_cast<uint32_t>(hex - 'A' + 10);
                        else { valid = false; break; }
                    }
                    if (!valid) { fail("invalid \\u escape"); break; }
                    if (codeUnit >= 0xD800u && codeUnit <= 0xDBFFu && pos + 1 < text.size() &&
                        text[pos] == '\\' && text[pos + 1] == 'u') {
                        // Combine a surrogate pair.
                        pos += 2;
                        uint32_t low = 0;
                        for (int i = 0; i < 4; ++i) {
                            if (eof()) { low = 0; break; }
                            const char hex = text[pos++];
                            low <<= 4u;
                            if (hex >= '0' && hex <= '9') low |= static_cast<uint32_t>(hex - '0');
                            else if (hex >= 'a' && hex <= 'f') low |= static_cast<uint32_t>(hex - 'a' + 10);
                            else if (hex >= 'A' && hex <= 'F') low |= static_cast<uint32_t>(hex - 'A' + 10);
                            else { low = 0; break; }
                        }
                        if (low >= 0xDC00u && low <= 0xDFFFu) {
                            codeUnit = 0x10000u + ((codeUnit - 0xD800u) << 10u) + (low - 0xDC00u);
                        } else {
                            fail("invalid low surrogate");
                        }
                    }
                    append_utf8(out, codeUnit);
                    break;
                }
                default:
                    fail("unknown escape sequence");
                    break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    Value parse_number() {
        const std::size_t start = pos;
        if (peek() == '-') ++pos;
        while (!eof() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        if (peek() == '.') {
            ++pos;
            while (!eof() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos;
            if (peek() == '+' || peek() == '-') ++pos;
            while (!eof() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        }
        const std::string token = text.substr(start, pos - start);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == token.c_str()) {
            fail("invalid number");
            return Value();
        }
        return Value(value);
    }

    Value parse_array() {
        Value array = Value::make_array();
        ++pos; // '['
        skip_whitespace();
        if (consume(']')) return array;
        while (true) {
            skip_whitespace();
            array.push(parse_value());
            skip_whitespace();
            if (consume(']')) break;
            if (!consume(',')) { fail("expected ',' or ']' in array"); break; }
        }
        return array;
    }

    Value parse_object() {
        Value object = Value::make_object();
        ++pos; // '{'
        skip_whitespace();
        if (consume('}')) return object;
        while (true) {
            skip_whitespace();
            if (peek() != '"') { fail("expected object key string"); break; }
            std::string key = parse_string();
            skip_whitespace();
            if (!consume(':')) { fail("expected ':' after object key"); break; }
            skip_whitespace();
            object[std::move(key)] = parse_value();
            skip_whitespace();
            if (consume('}')) break;
            if (!consume(',')) { fail("expected ',' or '}' in object"); break; }
        }
        return object;
    }

    Value parse_value() {
        skip_whitespace();
        const char c = peek();
        switch (c) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return Value(parse_string());
        case 't':
            if (match_literal("true")) return Value(true);
            fail("invalid literal");
            return Value();
        case 'f':
            if (match_literal("false")) return Value(false);
            fail("invalid literal");
            return Value();
        case 'n':
            if (match_literal("null")) return Value(nullptr);
            fail("invalid literal");
            return Value();
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
            fail("unexpected character");
            return Value();
        }
    }

    Value run() {
        skip_whitespace();
        if (eof()) {
            fail("empty JSON document");
            return Value();
        }
        Value root = parse_value();
        skip_whitespace();
        if (!eof() && error.empty()) fail("trailing content after JSON document");
        return root;
    }
};

} // namespace detail

inline Value parse(const std::string& text, std::string* error) {
    detail::Parser parser(text);
    Value result = parser.run();
    if (!parser.error.empty()) {
        if (error) *error = std::move(parser.error);
        return Value();
    }
    return result;
}

inline std::string stringify(const Value& v, int indent) {
    std::string out;
    detail::stringify_into(v, indent, 0, out);
    return out;
}

} // namespace Engine::Json
