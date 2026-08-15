#include "GltfGeometry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>

namespace Engine {

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON parser (objects, arrays, strings with escapes, numbers, literals)
// ---------------------------------------------------------------------------
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind{ Kind::Null };
    bool boolean{ false };
    double number{ 0 };
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(const std::string& key) const {
        if (kind != Kind::Object) return nullptr;
        for (const auto& [k, v] : object) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    const JsonValue* at(size_t index) const {
        return (kind == Kind::Array && index < array.size()) ? &array[index] : nullptr;
    }
    bool is_number() const { return kind == Kind::Number; }
    double as_number(double def = 0) const { return kind == Kind::Number ? number : def; }
    bool as_bool(bool def = false) const { return kind == Kind::Bool ? boolean : def; }
    const std::string* as_string() const { return kind == Kind::String ? &string : nullptr; }
    size_t array_size() const { return kind == Kind::Array ? array.size() : 0; }
};

class JsonParser final {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(JsonValue& out, std::string& error) {
        skip_ws();
        if (!parse_value(out, error)) return false;
        skip_ws();
        if (pos_ != text_.size()) {
            error = "trailing characters after JSON document";
            return false;
        }
        return true;
    }

private:
    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }
    char take() { return eof() ? '\0' : text_[pos_++]; }
    void skip_ws() {
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
    }

    bool parse_value(JsonValue& out, std::string& error) {
        skip_ws();
        if (eof()) { error = "unexpected end of JSON"; return false; }
        const char c = peek();
        if (c == '{') return parse_object(out, error);
        if (c == '[') return parse_array(out, error);
        if (c == '"') return parse_string(out.string, error), out.kind = JsonValue::Kind::String, true;
        if (c == 't' || c == 'f') return parse_literal(out, error);
        if (c == 'n') { parse_literal(out, error); return out.kind == JsonValue::Kind::Null ? true : (error = "invalid literal", false); }
        return parse_number(out, error);
    }

    bool parse_object(JsonValue& out, std::string& error) {
        take(); // '{'
        out.kind = JsonValue::Kind::Object;
        skip_ws();
        if (peek() == '}') { take(); return true; }
        while (true) {
            skip_ws();
            if (peek() != '"') { error = "expected string key in object"; return false; }
            std::string key;
            parse_string(key, error);
            skip_ws();
            if (take() != ':') { error = "expected ':' in object"; return false; }
            JsonValue value;
            if (!parse_value(value, error)) return false;
            out.object.emplace_back(std::move(key), std::move(value));
            skip_ws();
            const char c = take();
            if (c == ',') continue;
            if (c == '}') return true;
            error = "expected ',' or '}' in object (at " + std::to_string(pos_ - 1) + ")";
            return false;
        }
    }

    bool parse_array(JsonValue& out, std::string& error) {
        take(); // '['
        out.kind = JsonValue::Kind::Array;
        skip_ws();
        if (peek() == ']') { take(); return true; }
        while (true) {
            JsonValue value;
            if (!parse_value(value, error)) return false;
            out.array.push_back(std::move(value));
            skip_ws();
            const char c = take();
            if (c == ',') continue;
            if (c == ']') return true;
            error = "expected ',' or ']' in array";
            return false;
        }
    }

    void parse_string(std::string& out, std::string& error) {
        take(); // '"'
        out.clear();
        while (!eof()) {
            const char c = take();
            if (c == '"') return;
            if (c == '\\') {
                if (eof()) { error = "unterminated escape"; return; }
                const char e = take();
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) { error = "bad unicode escape"; return; }
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = take();
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                            else { error = "bad unicode hex digit"; return; }
                        }
                        // UTF-8 encode (only BMP; surrogate pairs collapse to U+FFFD).
                        if (code >= 0xD800 && code <= 0xDFFF) {
                            out.append("\xEF\xBF\xBD");
                        } else if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: error = "invalid escape"; return;
                }
            } else {
                out.push_back(c);
            }
        }
        error = "unterminated string";
    }

    bool parse_literal(JsonValue& out, std::string& error) {
        if (text_.compare(pos_, 4, "true") == 0) { pos_ += 4; out.kind = JsonValue::Kind::Bool; out.boolean = true; return true; }
        if (text_.compare(pos_, 5, "false") == 0) { pos_ += 5; out.kind = JsonValue::Kind::Bool; out.boolean = false; return true; }
        if (text_.compare(pos_, 4, "null") == 0) { pos_ += 4; out.kind = JsonValue::Kind::Null; return true; }
        error = "invalid literal";
        return false;
    }

    bool parse_number(JsonValue& out, std::string& error) {
        const size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (!eof() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' ||
                          peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-')) {
            ++pos_;
        }
        if (pos_ == start) { error = "invalid number"; return false; }
        char* end = nullptr;
        const std::string token(text_.substr(start, pos_ - start));
        out.number = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size()) { error = "invalid number"; return false; }
        out.kind = JsonValue::Kind::Number;
        return true;
    }

    std::string_view text_;
    size_t pos_{ 0 };
};

// ---------------------------------------------------------------------------
// Base64 decoding (glTF data URIs)
// ---------------------------------------------------------------------------
std::vector<uint8_t> base64_decode(const std::string& input) {
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        return t;
    }();
    std::vector<uint8_t> out;
    uint32_t accumulator = 0;
    int bits = 0;
    for (const unsigned char c : input) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int value = table[c];
        if (value < 0) continue;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// GLB container (header + JSON chunk + optional BIN chunk)
// ---------------------------------------------------------------------------
struct GlbData {
    std::string json;
    std::vector<uint8_t> bin;
};

bool parse_glb(const uint8_t* data, size_t size, GlbData& out, std::string& error) {
    const auto readU32 = [&](size_t offset) -> uint32_t {
        return static_cast<uint32_t>(data[offset]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
    };
    if (size < 20 || std::memcmp(data, "glTF", 4) != 0) {
        error = "invalid GLB header";
        return false;
    }
    const uint32_t version = readU32(4);
    const uint32_t length = readU32(8);
    if (version != 2 || length != size) {
        error = "unsupported GLB version or length mismatch";
        return false;
    }
    size_t offset = 12;
    bool sawJson = false;
    bool sawBin = false;
    while (offset + 8 <= size) {
        const uint32_t chunkLength = readU32(offset);
        const uint32_t chunkType = readU32(offset + 4);
        offset += 8;
        if (offset + chunkLength > size) {
            error = "GLB chunk exceeds file size";
            return false;
        }
        if (chunkType == 0x4E4F534A && !sawJson) {          // "JSON"
            out.json.assign(reinterpret_cast<const char*>(data + offset), chunkLength);
            sawJson = true;
        } else if (chunkType == 0x004E4942 && !sawBin) {    // "BIN\0"
            out.bin.assign(data + offset, data + offset + chunkLength);
            sawBin = true;
        }
        offset += chunkLength;
    }
    if (!sawJson || out.json.empty()) {
        error = "GLB has no JSON chunk";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Accessor / bufferView reading
// ---------------------------------------------------------------------------
struct ResolvedBuffer {
    const std::vector<uint8_t>* bytes{ nullptr };
    bool available{ false };
};

int component_bytes(int componentType) {
    switch (componentType) {
        case 5120: case 5121: return 1;   // BYTE / UBYTE
        case 5122: case 5123: return 2;   // SHORT / USHORT
        case 5125: case 5126: return 4;   // UINT / FLOAT
        default: return 0;
    }
}

int component_components(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

bool read_float_accessor(const JsonValue& accessor, const std::vector<ResolvedBuffer>& buffers,
                         const std::vector<JsonValue>& bufferViews,
                         int components, std::vector<float>& out, std::string& error) {
    const JsonValue* componentTypeJson = accessor.find("componentType");
    const JsonValue* countJson = accessor.find("count");
    if (!componentTypeJson || !countJson) {
        error = "accessor missing componentType or count";
        return false;
    }
    const int componentType = static_cast<int>(componentTypeJson->as_number(-1));
    const int bytes = component_bytes(componentType);
    if (bytes == 0) {
        error = "unsupported componentType " + std::to_string(componentType);
        return false;
    }
    const int count = static_cast<int>(countJson->as_number(0));
    const JsonValue* normalizedJson = accessor.find("normalized");
    const bool normalized = normalizedJson && normalizedJson->as_bool(false);
    const JsonValue* bufferView = accessor.find("bufferView");
    if (!bufferView) {
        error = "accessor without bufferView";
        return false;
    }
    const int bufferViewIndex = static_cast<int>(bufferView->as_number(-1));
    if (bufferViewIndex < 0 || bufferViewIndex >= static_cast<int>(bufferViews.size())) {
        error = "bufferView index out of range";
        return false;
    }
    const JsonValue& bv = bufferViews[static_cast<size_t>(bufferViewIndex)];
    const JsonValue* bufferRef = bv.find("buffer");
    if (!bufferRef) {
        error = "bufferView missing buffer";
        return false;
    }
    const int bufferIndex = static_cast<int>(bufferRef->as_number(-1));
    if (bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers.size()) || !buffers[static_cast<size_t>(bufferIndex)].available) {
        error = "buffer not available (external buffers are not supported at runtime)";
        return false;
    }
    const std::vector<uint8_t>& buffer = *buffers[static_cast<size_t>(bufferIndex)].bytes;
    const JsonValue* bvOffsetJson = bv.find("byteOffset");
    const JsonValue* accOffsetJson = accessor.find("byteOffset");
    const size_t bufferViewOffset = bvOffsetJson ? static_cast<size_t>(bvOffsetJson->as_number(0)) : 0;
    const size_t accessorOffset = accOffsetJson ? static_cast<size_t>(accOffsetJson->as_number(0)) : 0;
    const JsonValue* strideJson = bv.find("byteStride");
    const size_t stride = strideJson
        ? static_cast<size_t>(strideJson->as_number(0))
        : static_cast<size_t>(bytes * components);

    out.clear();
    out.reserve(static_cast<size_t>(count) * static_cast<size_t>(components));
    const uint8_t* base = buffer.data() + bufferViewOffset + accessorOffset;

    for (int i = 0; i < count; ++i) {
        const uint8_t* src = base + static_cast<size_t>(i) * stride;
        for (int c = 0; c < components; ++c) {
            float value = 0.0f;
            switch (componentType) {
                case 5126: { float v; std::memcpy(&v, src + static_cast<size_t>(c) * 4, 4); value = v; break; }
                case 5125: { uint32_t v; std::memcpy(&v, src + static_cast<size_t>(c) * 4, 4); value = static_cast<float>(v); break; }
                case 5123: { uint16_t v; std::memcpy(&v, src + static_cast<size_t>(c) * 2, 2); value = static_cast<float>(v); break; }
                case 5122: { int16_t v; std::memcpy(&v, src + static_cast<size_t>(c) * 2, 2); value = static_cast<float>(v); break; }
                case 5121: value = static_cast<float>(src[c]); break;
                case 5120: value = static_cast<float>(static_cast<int8_t>(src[c])); break;
                default: break;
            }
            if (normalized) {
                switch (componentType) {
                    case 5120: value = std::max(-1.0f, value / 127.0f); break;
                    case 5122: value = std::max(-1.0f, value / 32767.0f); break;
                    case 5121: value = value / 255.0f; break;
                    case 5123: value = value / 65535.0f; break;
                    default: break;
                }
            }
            out.push_back(value);
        }
    }
    return true;
}

bool read_index_accessor(const JsonValue& accessor, const std::vector<ResolvedBuffer>& buffers,
                         const std::vector<JsonValue>& bufferViews,
                         std::vector<uint32_t>& out, std::string& error) {
    const JsonValue* componentTypeJson = accessor.find("componentType");
    const JsonValue* countJson = accessor.find("count");
    if (!componentTypeJson || !countJson) {
        error = "index accessor missing componentType or count";
        return false;
    }
    const int componentType = static_cast<int>(componentTypeJson->as_number(-1));
    if (componentType != 5121 && componentType != 5123 && componentType != 5125) {
        error = "indices must use UBYTE, USHORT or UINT";
        return false;
    }
    const int count = static_cast<int>(countJson->as_number(0));
    const JsonValue* bufferView = accessor.find("bufferView");
    if (!bufferView) {
        error = "index accessor without bufferView";
        return false;
    }
    const int bufferViewIndex = static_cast<int>(bufferView->as_number(-1));
    if (bufferViewIndex < 0 || bufferViewIndex >= static_cast<int>(bufferViews.size())) {
        error = "index bufferView index out of range";
        return false;
    }
    const JsonValue& bv = bufferViews[static_cast<size_t>(bufferViewIndex)];
    const JsonValue* bufferRef = bv.find("buffer");
    if (!bufferRef) {
        error = "index bufferView missing buffer";
        return false;
    }
    const int bufferIndex = static_cast<int>(bufferRef->as_number(-1));
    if (bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers.size()) || !buffers[static_cast<size_t>(bufferIndex)].available) {
        error = "buffer not available for indices";
        return false;
    }
    const std::vector<uint8_t>& buffer = *buffers[static_cast<size_t>(bufferIndex)].bytes;
    const JsonValue* bvOffsetJson = bv.find("byteOffset");
    const JsonValue* accOffsetJson = accessor.find("byteOffset");
    const size_t offset = (bvOffsetJson ? static_cast<size_t>(bvOffsetJson->as_number(0)) : 0) +
                          (accOffsetJson ? static_cast<size_t>(accOffsetJson->as_number(0)) : 0);
    out.clear();
    out.reserve(static_cast<size_t>(count));
    const uint8_t* base = buffer.data() + offset;
    for (int i = 0; i < count; ++i) {
        if (componentType == 5121) out.push_back(base[i]);
        else if (componentType == 5123) { uint16_t v; std::memcpy(&v, base + static_cast<size_t>(i) * 2, 2); out.push_back(v); }
        else { uint32_t v; std::memcpy(&v, base + static_cast<size_t>(i) * 4, 4); out.push_back(v); }
    }
    return true;
}

// Reads a VEC4 integer accessor (glTF JOINTS_0): UBYTE, USHORT or UINT
// component types, honoring bufferView byteOffset/byteStride.
bool read_uvec4_accessor(const JsonValue& accessor, const std::vector<ResolvedBuffer>& buffers,
                         const std::vector<JsonValue>& bufferViews,
                         std::vector<glm::uvec4>& out, std::string& error) {
    const JsonValue* componentTypeJson = accessor.find("componentType");
    const JsonValue* countJson = accessor.find("count");
    if (!componentTypeJson || !countJson) {
        error = "joints accessor missing componentType or count";
        return false;
    }
    const int componentType = static_cast<int>(componentTypeJson->as_number(-1));
    if (componentType != 5121 && componentType != 5123 && componentType != 5125) {
        error = "joints must use UBYTE, USHORT or UINT";
        return false;
    }
    const int count = static_cast<int>(countJson->as_number(0));
    const JsonValue* bufferView = accessor.find("bufferView");
    if (!bufferView) {
        error = "joints accessor without bufferView";
        return false;
    }
    const int bufferViewIndex = static_cast<int>(bufferView->as_number(-1));
    if (bufferViewIndex < 0 || bufferViewIndex >= static_cast<int>(bufferViews.size())) {
        error = "joints bufferView index out of range";
        return false;
    }
    const JsonValue& bv = bufferViews[static_cast<size_t>(bufferViewIndex)];
    const JsonValue* bufferRef = bv.find("buffer");
    if (!bufferRef) {
        error = "joints bufferView missing buffer";
        return false;
    }
    const int bufferIndex = static_cast<int>(bufferRef->as_number(-1));
    if (bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers.size()) || !buffers[static_cast<size_t>(bufferIndex)].available) {
        error = "buffer not available for joints";
        return false;
    }
    const std::vector<uint8_t>& buffer = *buffers[static_cast<size_t>(bufferIndex)].bytes;
    const JsonValue* bvOffsetJson = bv.find("byteOffset");
    const JsonValue* accOffsetJson = accessor.find("byteOffset");
    const size_t bufferViewOffset = bvOffsetJson ? static_cast<size_t>(bvOffsetJson->as_number(0)) : 0;
    const size_t accessorOffset = accOffsetJson ? static_cast<size_t>(accOffsetJson->as_number(0)) : 0;
    const JsonValue* strideJson = bv.find("byteStride");
    const size_t stride = strideJson
        ? static_cast<size_t>(strideJson->as_number(0))
        : 16;
    out.clear();
    out.reserve(static_cast<size_t>(count));
    const uint8_t* base = buffer.data() + bufferViewOffset + accessorOffset;
    for (int i = 0; i < count; ++i) {
        const uint8_t* src = base + static_cast<size_t>(i) * stride;
        glm::uvec4 j(0);
        for (int c = 0; c < 4; ++c) {
            if (componentType == 5121) j[c] = src[c];
            else if (componentType == 5123) { uint16_t v; std::memcpy(&v, src + static_cast<size_t>(c) * 2, 2); j[c] = v; }
            else { uint32_t v; std::memcpy(&v, src + static_cast<size_t>(c) * 4, 4); j[c] = v; }
        }
        out.push_back(j);
    }
    return true;
}

// Computes flat per-vertex normals for a triangle list (indexed or not).
void compute_flat_normals(GltfMeshPrimitive& primitive) {
    const size_t count = primitive.positions.size();
    primitive.normals.assign(count, glm::vec3(0.0f));
    const auto normal_for = [&](size_t a, size_t b, size_t c) {
        if (a >= count || b >= count || c >= count) return;
        const glm::vec3 n = glm::cross(primitive.positions[b] - primitive.positions[a],
                                       primitive.positions[c] - primitive.positions[a]);
        const float len = glm::length(n);
        if (len < 1e-12f) return;
        const glm::vec3 unit = n / len;
        primitive.normals[a] += unit;
        primitive.normals[b] += unit;
        primitive.normals[c] += unit;
    };
    if (primitive.indexed) {
        for (size_t i = 0; i + 2 < primitive.indices.size(); i += 3) {
            normal_for(primitive.indices[i], primitive.indices[i + 1], primitive.indices[i + 2]);
        }
    } else {
        for (size_t i = 0; i + 2 < count; i += 3) {
            normal_for(i, i + 1, i + 2);
        }
    }
    for (glm::vec3& n : primitive.normals) {
        const float len = glm::length(n);
        if (len > 1e-8f) n /= len;
        else n = glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

} // namespace

// ===========================================================================
// GltfGeometryParser
// ===========================================================================

GltfGeometryResult GltfGeometryParser::parse(const std::vector<uint8_t>& bytes, std::string* error) {
    GltfGeometryResult result;
    const auto fail = [&](const std::string& message) {
        result.success = false;
        result.error = message;
        if (error) *error = message;
        return result;
    };

    std::string jsonText;
    std::vector<uint8_t> binChunk;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        GlbData glb;
        if (!parse_glb(bytes.data(), bytes.size(), glb, result.error)) {
            if (error) *error = result.error;
            return result;
        }
        jsonText = std::move(glb.json);
        binChunk = std::move(glb.bin);
    } else {
        jsonText.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    JsonParser parser(jsonText);
    JsonValue root;
    if (!parser.parse(root, result.error)) {
        if (error) *error = result.error;
        return result;
    }
    const JsonValue* buffersJson = root.find("buffers");
    const JsonValue* bufferViewsJson = root.find("bufferViews");
    const JsonValue* accessorsJson = root.find("accessors");
    const JsonValue* meshesJson = root.find("meshes");
    if (!buffersJson || !meshesJson) return fail("glTF must contain buffers and meshes");
    if (!accessorsJson || accessorsJson->array_size() == 0) return fail("glTF has no accessors");

    // Resolve buffers: data URIs or the GLB BIN chunk (buffer 0 without uri).
    std::vector<std::vector<uint8_t>> bufferStorage;
    std::vector<ResolvedBuffer> buffers;
    for (size_t i = 0; i < buffersJson->array_size(); ++i) {
        const JsonValue* buffer = buffersJson->at(i);
        if (!buffer) return fail("invalid buffer entry");
        const std::string* uri = buffer->find("uri") ? buffer->find("uri")->as_string() : nullptr;
        if (uri) {
            const std::string marker = ";base64,";
            const size_t markerPos = uri->find(marker);
            if (uri->rfind("data:", 0) != 0 || markerPos == std::string::npos) {
                return fail("external (non data-URI) buffers are not supported at runtime");
            }
            bufferStorage.emplace_back(base64_decode(uri->substr(markerPos + marker.size())));
            buffers.push_back({ &bufferStorage.back(), true });
        } else if (i == 0 && !binChunk.empty()) {
            bufferStorage.emplace_back(std::move(binChunk));
            buffers.push_back({ &bufferStorage.back(), true });
        } else {
            bufferStorage.emplace_back();
            buffers.push_back({ nullptr, false });
        }
    }
    if (bufferViewsJson == nullptr) return fail("glTF has no bufferViews");

    std::vector<JsonValue> bufferViews;
    for (size_t i = 0; i < bufferViewsJson->array_size(); ++i) {
        if (bufferViewsJson->at(i)) bufferViews.push_back(*bufferViewsJson->at(i));
    }
    std::vector<JsonValue> accessors;
    for (size_t i = 0; i < accessorsJson->array_size(); ++i) {
        if (accessorsJson->at(i)) accessors.push_back(*accessorsJson->at(i));
    }

    // Nodes: names + children (used to resolve skin joint parents).
    std::vector<std::string> nodeNames;
    std::vector<std::vector<uint32_t>> nodeChildren;
    if (const JsonValue* nodesJson = root.find("nodes")) {
        for (size_t i = 0; i < nodesJson->array_size(); ++i) {
            const JsonValue* node = nodesJson->at(i);
            if (!node) continue;
            const std::string* name = node->find("name") && node->find("name")->as_string()
                                          ? node->find("name")->as_string()
                                          : nullptr;
            nodeNames.push_back(name ? *name : "node");
            std::vector<uint32_t> children;
            if (const JsonValue* ch = node->find("children")) {
                for (const JsonValue& c : ch->array) {
                    if (c.kind == JsonValue::Kind::Number) children.push_back(static_cast<uint32_t>(c.number));
                }
            }
            nodeChildren.push_back(std::move(children));
        }
    }

    // Skins → skeletons (joint names, parents, inverse-bind matrices).
    if (const JsonValue* skinsJson = root.find("skins")) {
        for (size_t s = 0; s < skinsJson->array_size(); ++s) {
            const JsonValue* skinJson = skinsJson->at(s);
            if (!skinJson) continue;
            GltfGeometrySkin skin;
            if (const std::string* name = skinJson->find("name") ? skinJson->find("name")->as_string() : nullptr) {
                skin.name = *name;
            }
            std::vector<uint32_t> jointNodes;
            if (const JsonValue* joints = skinJson->find("joints")) {
                for (const JsonValue& j : joints->array) {
                    if (j.kind == JsonValue::Kind::Number) jointNodes.push_back(static_cast<uint32_t>(j.number));
                }
            }
            for (const uint32_t node : jointNodes) {
                skin.jointNames.push_back(node < nodeNames.size() ? nodeNames[node] : "joint" + std::to_string(node));
                // Parent: the joint whose children list contains this node.
                int32_t parent = -1;
                for (size_t j = 0; j < jointNodes.size(); ++j) {
                    if (jointNodes[j] >= nodeChildren.size()) continue;
                    for (const uint32_t child : nodeChildren[jointNodes[j]]) {
                        if (child == node) { parent = static_cast<int32_t>(j); break; }
                    }
                    if (parent >= 0) break;
                }
                skin.jointParents.push_back(parent);
            }
            // Inverse bind matrices (FLOAT accessor, 16 floats per matrix).
            skin.inverseBindMatrices.assign(jointNodes.size(), glm::mat4(1.0f));
            if (const JsonValue* ibm = skinJson->find("inverseBindMatrices")) {
                const int ibmIndex = static_cast<int>(ibm->as_number(-1));
                if (ibmIndex >= 0 && ibmIndex < static_cast<int>(accessors.size())) {
                    std::vector<float> floats;
                    std::string localError;
                    if (read_float_accessor(accessors[static_cast<size_t>(ibmIndex)], buffers, bufferViews,
                                            16, floats, localError)) {
                        for (size_t i = 0; i < skin.inverseBindMatrices.size() && (i + 1) * 16 <= floats.size(); ++i) {
                            glm::mat4& m = skin.inverseBindMatrices[i];
                            for (int c = 0; c < 4; ++c)
                                for (int r = 0; r < 4; ++r)
                                    m[c][r] = floats[i * 16 + static_cast<size_t>(c) * 4 + static_cast<size_t>(r)];
                        }
                    }
                }
            }
            if (!jointNodes.empty()) result.skins.push_back(std::move(skin));
        }
    }

    // Meshes → primitives.
    for (size_t m = 0; m < meshesJson->array_size(); ++m) {
        const JsonValue* mesh = meshesJson->at(m);
        if (!mesh) continue;
        const JsonValue* primitivesJson = mesh->find("primitives");
        if (!primitivesJson) continue;
        for (size_t p = 0; p < primitivesJson->array_size(); ++p) {
            const JsonValue* primitiveJson = primitivesJson->at(p);
            if (!primitiveJson) continue;
            GltfMeshPrimitive primitive;
            const JsonValue* attributes = primitiveJson->find("attributes");
            if (!attributes) continue;

            const auto readAttribute = [&](const char* name, int components, std::vector<float>& out) -> bool {
                const JsonValue* accessorRef = attributes->find(name);
                if (!accessorRef || !accessorRef->is_number()) return false;
                const int index = static_cast<int>(accessorRef->as_number(-1));
                if (index < 0 || index >= static_cast<int>(accessors.size())) return false;
                std::string localError;
                return read_float_accessor(accessors[static_cast<size_t>(index)], buffers, bufferViews,
                                           components, out, localError);
            };

            std::vector<float> positionData;
            std::vector<float> normalData;
            std::vector<float> uvData;
            if (!readAttribute("POSITION", 3, positionData)) {
                return fail("mesh primitive has no readable POSITION accessor");
            }
            const size_t vertexCount = positionData.size() / 3;
            primitive.positions.reserve(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) {
                primitive.positions.emplace_back(positionData[i * 3], positionData[i * 3 + 1], positionData[i * 3 + 2]);
            }
            if (readAttribute("NORMAL", 3, normalData) && normalData.size() == positionData.size()) {
                primitive.normals.reserve(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) {
                    primitive.normals.emplace_back(normalData[i * 3], normalData[i * 3 + 1], normalData[i * 3 + 2]);
                }
            } else {
                // Fill later with flat normals once indices are known.
            }
            if (readAttribute("TEXCOORD_0", 2, uvData) && uvData.size() >= vertexCount * 2) {
                primitive.uvs.reserve(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) {
                    primitive.uvs.emplace_back(uvData[i * 2], uvData[i * 2 + 1]);
                }
            }

            // Skinning attributes: JOINTS_0 (uvec4) + WEIGHTS_0 (vec4).
            if (const JsonValue* jointsRef = attributes->find("JOINTS_0")) {
                const int jointsIndex = static_cast<int>(jointsRef->as_number(-1));
                if (jointsIndex >= 0 && jointsIndex < static_cast<int>(accessors.size())) {
                    std::string localError;
                    if (read_uvec4_accessor(accessors[static_cast<size_t>(jointsIndex)], buffers,
                                            bufferViews, primitive.joints, localError) &&
                        primitive.joints.size() == vertexCount) {
                        std::vector<float> weightData;
                        if (readAttribute("WEIGHTS_0", 4, weightData) && weightData.size() >= vertexCount * 4) {
                            primitive.weights.reserve(vertexCount);
                            for (size_t i = 0; i < vertexCount; ++i) {
                                primitive.weights.emplace_back(
                                    weightData[i * 4], weightData[i * 4 + 1],
                                    weightData[i * 4 + 2], weightData[i * 4 + 3]);
                            }
                        } else {
                            primitive.joints.clear();
                        }
                    }
                }
            }

            const JsonValue* indicesRef = primitiveJson->find("indices");
            if (indicesRef && indicesRef->is_number()) {
                const int index = static_cast<int>(indicesRef->as_number(-1));
                if (index < 0 || index >= static_cast<int>(accessors.size())) {
                    return fail("indices accessor out of range");
                }
                std::string localError;
                if (!read_index_accessor(accessors[static_cast<size_t>(index)], buffers, bufferViews,
                                         primitive.indices, localError)) {
                    return fail("cannot read indices: " + localError);
                }
                primitive.indexed = !primitive.indices.empty();
            }

            if (primitive.normals.empty()) compute_flat_normals(primitive);
            if (primitive.indexed) {
                result.indexCount += static_cast<uint32_t>(primitive.indices.size());
            } else {
                result.indexCount += static_cast<uint32_t>(primitive.positions.size());
            }
            result.vertexCount += static_cast<uint32_t>(primitive.positions.size());
            result.primitives.push_back(std::move(primitive));
        }
    }

    if (result.primitives.empty()) return fail("no drawable primitives found");
    result.success = true;
    return result;
}

// ─── VCMESH v2: binary geometry payload (no JSON re-parse at load time) ───
namespace {

// Reads the common header; returns the format version or 0 on failure.
uint32_t read_vcmesh_header(std::ifstream& in, uint32_t& primitiveCount, uint32_t& vertexCount,
                            uint32_t& indexCount, uint64_t& payloadSize) {
    std::array<char, 6> magic{};
    in.read(magic.data(), magic.size());
    if (!in || std::string_view(magic.data(), magic.size()) != "VCMESH") return 0;
    uint32_t version = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&primitiveCount), sizeof(primitiveCount));
    in.read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
    in.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
    in.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));
    if (!in) return 0;
    return version;
}

// Decodes the v2 binary payload into primitives.
GltfGeometryResult decode_v2_payload(std::ifstream& in, uint32_t primitiveCount) {
    GltfGeometryResult result;
    result.primitives.reserve(primitiveCount);
    for (uint32_t p = 0; p < primitiveCount; ++p) {
        GltfMeshPrimitive primitive;
        uint32_t positionCount = 0, normalCount = 0, uvCount = 0, indexCount = 0;
        uint8_t indexed = 0;
        in.read(reinterpret_cast<char*>(&positionCount), sizeof(positionCount));
        in.read(reinterpret_cast<char*>(&normalCount), sizeof(normalCount));
        in.read(reinterpret_cast<char*>(&uvCount), sizeof(uvCount));
        in.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
        in.read(reinterpret_cast<char*>(&indexed), sizeof(indexed));
        if (!in || positionCount == 0 || positionCount > (1u << 24)) {
            result.error = "invalid v2 primitive header";
            return result;
        }
        primitive.positions.resize(positionCount);
        in.read(reinterpret_cast<char*>(primitive.positions.data()),
                static_cast<std::streamsize>(positionCount * sizeof(glm::vec3)));
        if (normalCount > 0 && normalCount <= positionCount) {
            primitive.normals.resize(normalCount);
            in.read(reinterpret_cast<char*>(primitive.normals.data()),
                    static_cast<std::streamsize>(normalCount * sizeof(glm::vec3)));
        }
        if (uvCount > 0 && uvCount <= positionCount) {
            primitive.uvs.resize(uvCount);
            in.read(reinterpret_cast<char*>(primitive.uvs.data()),
                    static_cast<std::streamsize>(uvCount * sizeof(glm::vec2)));
        }
        if (indexCount > 0 && indexCount <= (1u << 24)) {
            primitive.indices.resize(indexCount);
            in.read(reinterpret_cast<char*>(primitive.indices.data()),
                    static_cast<std::streamsize>(indexCount * sizeof(uint32_t)));
            primitive.indexed = indexed != 0;
        }
        if (!in) {
            result.error = "truncated v2 geometry payload";
            return result;
        }
        result.vertexCount += positionCount;
        result.indexCount += indexCount;
        result.primitives.push_back(std::move(primitive));
    }
    result.success = !result.primitives.empty();
    if (!result.success) result.error = "v2 payload contains no primitives";
    return result;
}

// Decodes the v3 binary payload into primitives (with skinning data) and
// embedded skeletons. Layout per primitive: counts (position/normal/uv/joint/
// weight/index + indexed flag), then positions, normals, uvs, joints, weights,
// indices; then skinCount and per-skin name/jointNames/jointParents/inverseBind.
GltfGeometryResult decode_v3_payload(std::ifstream& in, uint32_t primitiveCount) {
    GltfGeometryResult result;
    result.primitives.reserve(primitiveCount);
    for (uint32_t p = 0; p < primitiveCount; ++p) {
        GltfMeshPrimitive primitive;
        uint32_t positionCount = 0, normalCount = 0, uvCount = 0;
        uint32_t jointCount = 0, weightCount = 0, indexCount = 0;
        uint8_t indexed = 0;
        in.read(reinterpret_cast<char*>(&positionCount), sizeof(positionCount));
        in.read(reinterpret_cast<char*>(&normalCount), sizeof(normalCount));
        in.read(reinterpret_cast<char*>(&uvCount), sizeof(uvCount));
        in.read(reinterpret_cast<char*>(&jointCount), sizeof(jointCount));
        in.read(reinterpret_cast<char*>(&weightCount), sizeof(weightCount));
        in.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
        in.read(reinterpret_cast<char*>(&indexed), sizeof(indexed));
        if (!in || positionCount == 0 || positionCount > (1u << 24)) {
            result.error = "invalid v3 primitive header";
            return result;
        }
        primitive.positions.resize(positionCount);
        in.read(reinterpret_cast<char*>(primitive.positions.data()),
                static_cast<std::streamsize>(positionCount * sizeof(glm::vec3)));
        if (normalCount > 0 && normalCount <= positionCount) {
            primitive.normals.resize(normalCount);
            in.read(reinterpret_cast<char*>(primitive.normals.data()),
                    static_cast<std::streamsize>(normalCount * sizeof(glm::vec3)));
        }
        if (uvCount > 0 && uvCount <= positionCount) {
            primitive.uvs.resize(uvCount);
            in.read(reinterpret_cast<char*>(primitive.uvs.data()),
                    static_cast<std::streamsize>(uvCount * sizeof(glm::vec2)));
        }
        if (jointCount == positionCount) {
            primitive.joints.resize(jointCount);
            in.read(reinterpret_cast<char*>(primitive.joints.data()),
                    static_cast<std::streamsize>(jointCount * sizeof(glm::uvec4)));
        }
        if (weightCount == positionCount) {
            primitive.weights.resize(weightCount);
            in.read(reinterpret_cast<char*>(primitive.weights.data()),
                    static_cast<std::streamsize>(weightCount * sizeof(glm::vec4)));
        }
        if (indexCount > 0 && indexCount <= (1u << 24)) {
            primitive.indices.resize(indexCount);
            in.read(reinterpret_cast<char*>(primitive.indices.data()),
                    static_cast<std::streamsize>(indexCount * sizeof(uint32_t)));
            primitive.indexed = indexed != 0;
        }
        if (!in) {
            result.error = "truncated v3 geometry payload";
            return result;
        }
        result.vertexCount += positionCount;
        result.indexCount += indexCount;
        result.primitives.push_back(std::move(primitive));
    }
    uint32_t skinCount = 0;
    if (!in.read(reinterpret_cast<char*>(&skinCount), sizeof(skinCount))) {
        result.error = "truncated v3 skin count";
        return result;
    }
    for (uint32_t s = 0; s < skinCount; ++s) {
        GltfGeometrySkin skin;
        uint32_t nameLen = 0;
        in.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (nameLen > 256 || !in) {
            result.error = "invalid v3 skin name";
            return result;
        }
        if (nameLen > 0) {
            skin.name.resize(nameLen);
            in.read(skin.name.data(), static_cast<std::streamsize>(nameLen));
        }
        uint32_t jointCount = 0;
        in.read(reinterpret_cast<char*>(&jointCount), sizeof(jointCount));
        if (jointCount > 4096 || !in) {
            result.error = "invalid v3 skin joint count";
            return result;
        }
        for (uint32_t j = 0; j < jointCount; ++j) {
            uint32_t jNameLen = 0;
            in.read(reinterpret_cast<char*>(&jNameLen), sizeof(jNameLen));
            if (jNameLen > 256 || !in) {
                result.error = "invalid v3 joint name";
                return result;
            }
            std::string jointName;
            if (jNameLen > 0) {
                jointName.resize(jNameLen);
                in.read(jointName.data(), static_cast<std::streamsize>(jNameLen));
            }
            skin.jointNames.push_back(std::move(jointName));
        }
        skin.jointParents.resize(jointCount);
        if (jointCount > 0)
            in.read(reinterpret_cast<char*>(skin.jointParents.data()),
                    static_cast<std::streamsize>(jointCount * sizeof(int32_t)));
        skin.inverseBindMatrices.resize(jointCount);
        if (jointCount > 0)
            in.read(reinterpret_cast<char*>(skin.inverseBindMatrices.data()),
                    static_cast<std::streamsize>(jointCount * sizeof(glm::mat4)));
        if (!in) {
            result.error = "truncated v3 skin data";
            return result;
        }
        result.skins.push_back(std::move(skin));
    }
    result.success = !result.primitives.empty();
    if (!result.success) result.error = "v3 payload contains no primitives";
    return result;
}

} // namespace

GltfGeometryResult GltfGeometryParser::parse_vcmesh(const std::filesystem::path& cookedPath, std::string* error) {
    GltfGeometryResult result;
    std::ifstream in(cookedPath, std::ios::binary);
    if (!in) {
        const std::string message = "cannot open cooked mesh: " + cookedPath.string();
        result.error = message;
        if (error) *error = message;
        return result;
    }
    uint32_t primitiveCount = 0, vertexCount = 0, indexCount = 0;
    uint64_t payloadSize = 0;
    const uint32_t formatVersion = read_vcmesh_header(in, primitiveCount, vertexCount, indexCount, payloadSize);
    if (formatVersion == 0 || payloadSize == 0 || payloadSize > (1u << 30)) {
        const std::string message = "unsupported or corrupt VCMESH file: " + cookedPath.string();
        result.error = message;
        if (error) *error = message;
        return result;
    }
    if (formatVersion == 2) {
        result = decode_v2_payload(in, primitiveCount);
        if (!result.success && error) *error = result.error;
        return result;
    }
    if (formatVersion == 3) {
        result = decode_v3_payload(in, primitiveCount);
        if (!result.success && error) *error = result.error;
        return result;
    }
    if (formatVersion != 1) {
        const std::string message = "unsupported VCMESH format version: " + std::to_string(formatVersion);
        result.error = message;
        if (error) *error = message;
        return result;
    }
    std::vector<uint8_t> payload(static_cast<size_t>(payloadSize));
    in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payloadSize));
    if (!in) {
        const std::string message = "truncated VCMESH payload";
        result.error = message;
        if (error) *error = message;
        return result;
    }
    return parse(payload, error);
}

bool GltfGeometryParser::write_cooked(const std::filesystem::path& cookedPath,
                                      const GltfGeometryResult& geometry, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!geometry.success || geometry.primitives.empty()) return fail("cannot cook empty geometry");
    std::ofstream out(cookedPath, std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot create cooked mesh: " + cookedPath.string());
    out.write("VCMESH", 6);
    const uint32_t version = 3;
    const uint32_t primitiveCount = static_cast<uint32_t>(geometry.primitives.size());
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&primitiveCount), sizeof(primitiveCount));
    out.write(reinterpret_cast<const char*>(&geometry.vertexCount), sizeof(geometry.vertexCount));
    out.write(reinterpret_cast<const char*>(&geometry.indexCount), sizeof(geometry.indexCount));
    const uint64_t payloadOffset = static_cast<uint64_t>(out.tellp());
    const uint64_t payloadSize = 0;
    out.write(reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize));
    for (const GltfMeshPrimitive& primitive : geometry.primitives) {
        const uint32_t positionCount = static_cast<uint32_t>(primitive.positions.size());
        const uint32_t normalCount = static_cast<uint32_t>(primitive.normals.size());
        const uint32_t uvCount = static_cast<uint32_t>(primitive.uvs.size());
        const uint32_t jointCount = static_cast<uint32_t>(primitive.joints.size());
        const uint32_t weightCount = static_cast<uint32_t>(primitive.weights.size());
        const uint32_t indexCount = static_cast<uint32_t>(primitive.indices.size());
        const uint8_t indexed = primitive.indexed ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&positionCount), sizeof(positionCount));
        out.write(reinterpret_cast<const char*>(&normalCount), sizeof(normalCount));
        out.write(reinterpret_cast<const char*>(&uvCount), sizeof(uvCount));
        out.write(reinterpret_cast<const char*>(&jointCount), sizeof(jointCount));
        out.write(reinterpret_cast<const char*>(&weightCount), sizeof(weightCount));
        out.write(reinterpret_cast<const char*>(&indexCount), sizeof(indexCount));
        out.write(reinterpret_cast<const char*>(&indexed), sizeof(indexed));
        out.write(reinterpret_cast<const char*>(primitive.positions.data()),
                  static_cast<std::streamsize>(positionCount * sizeof(glm::vec3)));
        if (normalCount > 0)
            out.write(reinterpret_cast<const char*>(primitive.normals.data()),
                      static_cast<std::streamsize>(normalCount * sizeof(glm::vec3)));
        if (uvCount > 0)
            out.write(reinterpret_cast<const char*>(primitive.uvs.data()),
                      static_cast<std::streamsize>(uvCount * sizeof(glm::vec2)));
        if (jointCount > 0)
            out.write(reinterpret_cast<const char*>(primitive.joints.data()),
                      static_cast<std::streamsize>(jointCount * sizeof(glm::uvec4)));
        if (weightCount > 0)
            out.write(reinterpret_cast<const char*>(primitive.weights.data()),
                      static_cast<std::streamsize>(weightCount * sizeof(glm::vec4)));
        if (indexCount > 0)
            out.write(reinterpret_cast<const char*>(primitive.indices.data()),
                      static_cast<std::streamsize>(indexCount * sizeof(uint32_t)));
    }
    const uint32_t skinCount = static_cast<uint32_t>(geometry.skins.size());
    out.write(reinterpret_cast<const char*>(&skinCount), sizeof(skinCount));
    for (const GltfGeometrySkin& skin : geometry.skins) {
        const uint32_t nameLen = static_cast<uint32_t>(skin.name.size());
        out.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        if (nameLen > 0)
            out.write(skin.name.data(), static_cast<std::streamsize>(nameLen));
        const uint32_t jointCount = static_cast<uint32_t>(skin.jointNames.size());
        out.write(reinterpret_cast<const char*>(&jointCount), sizeof(jointCount));
        for (const std::string& jointName : skin.jointNames) {
            const uint32_t jNameLen = static_cast<uint32_t>(jointName.size());
            out.write(reinterpret_cast<const char*>(&jNameLen), sizeof(jNameLen));
            if (jNameLen > 0)
                out.write(jointName.data(), static_cast<std::streamsize>(jNameLen));
        }
        if (jointCount > 0)
            out.write(reinterpret_cast<const char*>(skin.jointParents.data()),
                      static_cast<std::streamsize>(jointCount * sizeof(int32_t)));
        if (jointCount > 0)
            out.write(reinterpret_cast<const char*>(skin.inverseBindMatrices.data()),
                      static_cast<std::streamsize>(jointCount * sizeof(glm::mat4)));
    }
    const uint64_t finalPayloadSize = static_cast<uint64_t>(out.tellp()) - payloadOffset - sizeof(uint64_t);
    out.seekp(static_cast<std::streamoff>(payloadOffset));
    out.write(reinterpret_cast<const char*>(&finalPayloadSize), sizeof(finalPayloadSize));
    out.seekp(0, std::ios::end);
    if (!out) return fail("cannot write cooked mesh payload");
    return true;
}

} // namespace Engine
