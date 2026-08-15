#include "GltfAssets.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Engine {

namespace {

// Tiny JSON value parser (objects, arrays, strings, numbers, bools, null).
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind{Kind::Null};
    bool boolean{false};
    double number{0};
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    [[nodiscard]] const JsonValue* find(const std::string& key) const {
        if (kind != Kind::Object) return nullptr;
        for (const auto& [k, v] : object) if (k == key) return &v;
        return nullptr;
    }
    [[nodiscard]] const JsonValue* at(size_t i) const {
        if (kind != Kind::Array || i >= array.size()) return nullptr;
        return &array[i];
    }
    [[nodiscard]] std::optional<double> as_number() const {
        if (kind == Kind::Number) return number;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<bool> as_bool() const {
        if (kind == Kind::Bool) return boolean;
        return std::nullopt;
    }
    [[nodiscard]] const std::string* as_string() const {
        return (kind == Kind::String) ? &string : nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    [[nodiscard]] bool parse(JsonValue& out, std::string* error) {
        skip_ws();
        if (!parse_value(out)) {
            if (error) *error = "JSON syntax error at offset " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    std::string_view text_;
    size_t pos_{0};

    void skip_ws() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    [[nodiscard]] char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    [[nodiscard]] char get() { return pos_ < text_.size() ? text_[pos_++] : '\0'; }

    bool parse_value(JsonValue& out) {
        skip_ws();
        const char c = peek();
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') { out.kind = JsonValue::Kind::String; return parse_string(out.string); }
        if (c == 't' || c == 'f') { out.kind = JsonValue::Kind::Bool; return parse_bool(out.boolean); }
        if (c == 'n') { out.kind = JsonValue::Kind::Null; return consume("null"); }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) { out.kind = JsonValue::Kind::Number; return parse_number(out.number); }
        return false;
    }

    bool parse_string(std::string& out) {
        if (get() != '"') return false;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = get();
            if (c == '"') return true;
            if (c == '\\') {
                const char e = get();
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case '"': out += '"'; break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) return false;
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = get();
                            code = (code << 4) | (h <= '9' ? static_cast<unsigned>(h - '0')
                                                           : (h | 32) - 'a' + 10);
                        }
                        out += static_cast<char>(code);
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;
    }

    bool parse_number(double& out) {
        const size_t start = pos_;
        if (peek() == '-') get();
        while (std::isdigit(static_cast<unsigned char>(peek()))) get();
        if (peek() == '.') { get(); while (std::isdigit(static_cast<unsigned char>(peek()))) get(); }
        if (peek() == 'e' || peek() == 'E') {
            get();
            if (peek() == '+' || peek() == '-') get();
            while (std::isdigit(static_cast<unsigned char>(peek()))) get();
        }
        out = std::strtod(std::string(text_.substr(start, pos_ - start)).c_str(), nullptr);
        return true;
    }

    bool parse_bool(bool& out) {
        if (peek() == 't') { out = true; return consume("true"); }
        out = false;
        return consume("false");
    }

    bool consume(const char* lit) {
        const size_t len = std::strlen(lit);
        if (pos_ + len > text_.size() || text_.substr(pos_, len) != lit) return false;
        pos_ += len;
        return true;
    }

    bool parse_array(JsonValue& out) {
        if (get() != '[') return false;
        out.kind = JsonValue::Kind::Array;
        skip_ws();
        if (peek() == ']') { get(); return true; }
        for (;;) {
            JsonValue item;
            if (!parse_value(item)) return false;
            out.array.push_back(std::move(item));
            skip_ws();
            const char c = get();
            if (c == ']') return true;
            if (c != ',') return false;
        }
    }

    bool parse_object(JsonValue& out) {
        if (get() != '{') return false;
        out.kind = JsonValue::Kind::Object;
        skip_ws();
        if (peek() == '}') { get(); return true; }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (get() != ':') return false;
            JsonValue value;
            if (!parse_value(value)) return false;
            out.object.emplace_back(std::move(key), std::move(value));
            skip_ws();
            const char c = get();
            if (c == '}') return true;
            if (c != ',') return false;
        }
    }
};

// Reads a float array from a glTF accessor (either inline or from a GLB BIN
// chunk, which we skip — inline values only).
bool read_floats(const JsonValue& value, std::vector<float>& out) {
    if (value.kind == JsonValue::Kind::Array) {
        for (const JsonValue& v : value.array) {
            if (v.kind != JsonValue::Kind::Number) return false;
            out.push_back(static_cast<float>(v.number));
        }
        return true;
    }
    return false;
}

// Decodes a base64 payload (data-URI buffers).
std::vector<uint8_t> base64_decode(const std::string& input) {
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        return t;
    }();
    std::vector<uint8_t> out;
    int buffer = 0, bits = 0;
    for (const char c : input) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int value = table[static_cast<unsigned char>(c)];
        if (value < 0) break;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

struct ResolvedAnimBuffer {
    const std::vector<uint8_t>* bytes{ nullptr };
    bool available{ false };
};

// Reads a float accessor (animation times/TRS outputs) honoring bufferView
// byteOffset/byteStride and accessor byteOffset, with normalization.
bool read_anim_float_accessor(const JsonValue& accessor,
                              const std::vector<ResolvedAnimBuffer>& buffers,
                              const std::vector<JsonValue>& bufferViews,
                              int components, std::vector<float>& out) {
    const JsonValue* componentTypeJson = accessor.find("componentType");
    const JsonValue* countJson = accessor.find("count");
    if (!componentTypeJson || !countJson) return false;
    const int componentType = static_cast<int>(componentTypeJson->as_number().value_or(0));
    int bytes = 0;
    switch (componentType) {
        case 5120: case 5121: bytes = 1; break;
        case 5122: case 5123: bytes = 2; break;
        case 5125: case 5126: bytes = 4; break;
        default: return false;
    }
    const int count = static_cast<int>(countJson->as_number().value_or(0));
    const bool normalized = accessor.find("normalized") && accessor.find("normalized")->as_bool().value_or(false);
    const JsonValue* bufferView = accessor.find("bufferView");
    if (!bufferView) return false;
    const int bufferViewIndex = static_cast<int>(bufferView->as_number().value_or(-1));
    if (bufferViewIndex < 0 || bufferViewIndex >= static_cast<int>(bufferViews.size())) return false;
    const JsonValue& bv = bufferViews[static_cast<size_t>(bufferViewIndex)];
    const JsonValue* bufferRef = bv.find("buffer");
    if (!bufferRef) return false;
    const int bufferIndex = static_cast<int>(bufferRef->as_number().value_or(-1));
    if (bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers.size()) ||
        !buffers[static_cast<size_t>(bufferIndex)].available) {
        return false;
    }
    const std::vector<uint8_t>& buffer = *buffers[static_cast<size_t>(bufferIndex)].bytes;
    const size_t bvOffset = bv.find("byteOffset")
        ? static_cast<size_t>(bv.find("byteOffset")->as_number().value_or(0)) : 0;
    const size_t accOffset = accessor.find("byteOffset")
        ? static_cast<size_t>(accessor.find("byteOffset")->as_number().value_or(0)) : 0;
    const size_t stride = bv.find("byteStride")
        ? static_cast<size_t>(bv.find("byteStride")->as_number().value_or(0))
        : static_cast<size_t>(bytes) * static_cast<size_t>(components);
    out.clear();
    out.reserve(static_cast<size_t>(count) * static_cast<size_t>(components));
    const uint8_t* base = buffer.data() + bvOffset + accOffset;
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

// Parses "TRS" on a node: translation/rotation/scale.
bool parse_node_trs(const JsonValue& node, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    t = glm::vec3(0); r = glm::quat(1, 0, 0, 0); s = glm::vec3(1);
    if (const JsonValue* v = node.find("translation")) {
        std::vector<float> f;
        if (read_floats(*v, f) && f.size() >= 3) t = glm::vec3(f[0], f[1], f[2]);
    }
    if (const JsonValue* v = node.find("rotation")) {
        std::vector<float> f;
        if (read_floats(*v, f) && f.size() >= 4) r = glm::quat(f[3], f[0], f[1], f[2]);
    }
    if (const JsonValue* v = node.find("scale")) {
        std::vector<float> f;
        if (read_floats(*v, f) && f.size() >= 3) s = glm::vec3(f[0], f[1], f[2]);
    }
    return true;
}

glm::mat4 trs_to_matrix(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    glm::mat4 m = glm::mat4(1.0f);
    m = glm::translate(m, t);
    m = m * glm::mat4_cast(r);
    m = glm::scale(m, s);
    return m;
}

} // namespace

GltfParser::GltfParser(std::string json, std::vector<uint8_t> bin)
    : json_(std::move(json)), bin_(std::move(bin)) {}

bool GltfParser::parse(std::string* error) {
    JsonValue root;
    JsonParser parser(json_);
    if (!parser.parse(root, error)) return false;
    if (root.kind != JsonValue::Kind::Object) {
        if (error) *error = "glTF JSON root must be an object";
        return false;
    }

    // Buffers (data URIs or the GLB BIN chunk) + bufferViews + accessors, for
    // animation sampler input/output reading (standard glTF stores these in
    // binary accessors).
    std::vector<std::vector<uint8_t>> bufferStorage;
    std::vector<ResolvedAnimBuffer> buffers;
    if (const JsonValue* buffersJson = root.find("buffers")) {
        for (size_t i = 0; i < buffersJson->array.size(); ++i) {
            const JsonValue& buffer = buffersJson->array[i];
            const std::string* uri = buffer.find("uri") ? buffer.find("uri")->as_string() : nullptr;
            if (uri) {
                const std::string marker = ";base64,";
                const size_t markerPos = uri->find(marker);
                if (uri->rfind("data:", 0) == 0 && markerPos != std::string::npos) {
                    bufferStorage.push_back(base64_decode(uri->substr(markerPos + marker.size())));
                    buffers.push_back({ &bufferStorage.back(), true });
                    continue;
                }
            } else if (i == 0 && !bin_.empty()) {
                bufferStorage.push_back(bin_);
                buffers.push_back({ &bufferStorage.back(), true });
                continue;
            }
            bufferStorage.emplace_back();
            buffers.push_back({ nullptr, false });
        }
    }
    std::vector<JsonValue> bufferViews;
    if (const JsonValue* viewsJson = root.find("bufferViews")) {
        for (const JsonValue& view : viewsJson->array) bufferViews.push_back(view);
    }
    std::vector<JsonValue> accessors;
    if (const JsonValue* accessorsJson = root.find("accessors")) {
        for (const JsonValue& accessor : accessorsJson->array) accessors.push_back(accessor);
    }

    // Nodes.
    if (const JsonValue* nodes = root.find("nodes")) {
        for (const JsonValue& n : nodes->array) {
            nodeNames_.push_back(n.find("name") && n.find("name")->as_string()
                                     ? *n.find("name")->as_string()
                                     : "node");
            std::vector<uint32_t> children;
            if (const JsonValue* ch = n.find("children")) {
                for (const JsonValue& c : ch->array) {
                    if (c.as_number()) children.push_back(static_cast<uint32_t>(*c.as_number()));
                }
            }
            nodeChildren_.push_back(std::move(children));
        }
    }

    // Skins.
    if (const JsonValue* skins = root.find("skins")) {
        for (const JsonValue& s : skins->array) {
            GltfSkin skin;
            if (const JsonValue* name = s.find("name")) {
                if (const std::string* n = name->as_string()) skin.name = *n;
            }
            if (const JsonValue* joints = s.find("joints")) {
                for (const JsonValue& j : joints->array) {
                    if (j.as_number()) skin.jointNodes.push_back(static_cast<uint32_t>(*j.as_number()));
                }
            }
            // Inverse bind matrices are stored in accessors (binary); we emit
            // identity matrices so skeletal structure is still usable.
            skin.inverseBindMatrices.assign(skin.jointNodes.size(), glm::mat4(1.0f));
            skins_.push_back(std::move(skin));
        }
    }

    // Animations: TRS channels keyed by sampler. Samplers read their input
    // (times) and output (TRS values) through accessors (standard glTF) or
    // fall back to inline arrays.
    if (const JsonValue* animations = root.find("animations")) {
        for (const JsonValue& anim : animations->array) {
            GltfAnimationClip clip;
            if (const JsonValue* name = anim.find("name")) {
                if (const std::string* n = name->as_string()) clip.name = *n;
            }
            struct Sampler {
                std::vector<float> times;
                std::vector<float> output;
                bool valid{ false };
            };
            std::vector<Sampler> samplers;
            if (const JsonValue* smp = anim.find("samplers")) {
                for (const JsonValue& s : smp->array) {
                    Sampler sampler;
                    // Standard glTF: input/output are accessor indices.
                    bool read = false;
                    if (const JsonValue* input = s.find("input")) {
                        if (input->as_number()) {
                            const int index = static_cast<int>(*input->as_number());
                            if (index >= 0 && index < static_cast<int>(accessors.size())) {
                                read = read_anim_float_accessor(accessors[static_cast<size_t>(index)],
                                                                buffers, bufferViews, 1, sampler.times);
                            }
                        }
                    }
                    if (!read) {
                        if (const JsonValue* times = s.find("times")) read_floats(*times, sampler.times);
                    }
                    if (const JsonValue* out = s.find("output")) {
                        if (out->as_number()) {
                            const int index = static_cast<int>(*out->as_number());
                            if (index >= 0 && index < static_cast<int>(accessors.size())) {
                                // Component count comes from the accessor type
                                // (SCALAR=1, VEC3=3, VEC4=4) — parse it so the
                                // default (tightly packed) stride is right.
                                int components = 4;
                                if (const JsonValue* type = accessors[static_cast<size_t>(index)].find("type")) {
                                    if (const std::string* t = type->as_string()) {
                                        if (*t == "SCALAR") components = 1;
                                        else if (*t == "VEC2") components = 2;
                                        else if (*t == "VEC3") components = 3;
                                    }
                                }
                                read = read_anim_float_accessor(accessors[static_cast<size_t>(index)],
                                                                buffers, bufferViews, components, sampler.output);
                            }
                        } else {
                            read = read_floats(*out, sampler.output);
                        }
                    }
                    sampler.valid = read && !sampler.times.empty() && !sampler.output.empty();
                    samplers.push_back(std::move(sampler));
                }
            }
            if (const JsonValue* channels = anim.find("channels")) {
                for (const JsonValue& ch : channels->array) {
                    const JsonValue* target = ch.find("target");
                    if (!target) continue;
                    const std::string* path = target->find("path") ? target->find("path")->as_string() : nullptr;
                    if (!path) continue;
                    const JsonValue* samplerIdx = ch.find("sampler");
                    const int idx = samplerIdx && samplerIdx->as_number()
                                        ? static_cast<int>(*samplerIdx->as_number())
                                        : -1;
                    if (idx < 0 || idx >= static_cast<int>(samplers.size()) || !samplers[static_cast<size_t>(idx)].valid) continue;
                    const Sampler& sampler = samplers[static_cast<size_t>(idx)];
                    const int nodeIndex = target->find("node") && target->find("node")->as_number()
                                              ? static_cast<int>(*target->find("node")->as_number())
                                              : -1;
                    GltfAnimationChannel channel;
                    channel.nodeIndex = nodeIndex;
                    channel.nodeName = (nodeIndex >= 0 && nodeIndex < static_cast<int>(nodeNames_.size()))
                                           ? nodeNames_[static_cast<size_t>(nodeIndex)]
                                           : "root";
                    channel.times = sampler.times;
                    if (*path == "translation") {
                        for (size_t i = 0; i + 2 < sampler.output.size(); i += 3)
                            channel.translations.push_back(glm::vec3(sampler.output[i], sampler.output[i + 1], sampler.output[i + 2]));
                    } else if (*path == "rotation") {
                        for (size_t i = 0; i + 3 < sampler.output.size(); i += 4)
                            channel.rotations.push_back(glm::quat(sampler.output[i + 3], sampler.output[i], sampler.output[i + 1], sampler.output[i + 2]));
                    } else if (*path == "scale") {
                        for (size_t i = 0; i + 2 < sampler.output.size(); i += 3)
                            channel.scales.push_back(glm::vec3(sampler.output[i], sampler.output[i + 1], sampler.output[i + 2]));
                    }
                    if (!channel.times.empty()) {
                        clip.duration = std::max(clip.duration, channel.times.back());
                        clip.channels.push_back(std::move(channel));
                    }
                }
            }
            if (!clip.channels.empty()) clips_.push_back(std::move(clip));
        }
    }

    parsed_ = true;
    return true;
}

SkeletonAsset GltfParser::make_skeleton() const {
    SkeletonAsset skeleton;
    if (skins_.empty()) {
        BoneNode root;
        root.name = "root";
        root.parentIndex = -1;
        root.localTransform = glm::mat4(1.0f);
        skeleton.bones.push_back(std::move(root));
        return skeleton;
    }
    const GltfSkin& skin = skins_[0];
    skeleton.bones.reserve(skin.jointNodes.size());
    for (size_t i = 0; i < skin.jointNodes.size(); ++i) {
        const uint32_t node = skin.jointNodes[i];
        BoneNode bone;
        bone.name = node < nodeNames_.size() ? nodeNames_[node] : "bone" + std::to_string(i);
        bone.parentIndex = static_cast<int>(i) - 1;
        bone.localTransform = glm::mat4(1.0f);
        if (i < skin.inverseBindMatrices.size()) bone.inverseBindMatrix = skin.inverseBindMatrices[i];
        skeleton.bones.push_back(std::move(bone));
    }
    return skeleton;
}

AnimationClip GltfParser::make_clip() const {
    AnimationClip clip;
    if (clips_.empty()) return clip;
    const GltfAnimationClip& source = clips_[0];
    clip.name = source.name.empty() ? "clip" : source.name;
    clip.duration = source.duration;
    clip.looping = true;
    // Map channels to skeleton bones by node name, one BoneTrack per bone.
    const SkeletonAsset skeleton = make_skeleton();
    for (const GltfAnimationChannel& channel : source.channels) {
        int boneIndex = -1;
        for (size_t b = 0; b < skeleton.bones.size(); ++b) {
            if (skeleton.bones[b].name == channel.nodeName) {
                boneIndex = static_cast<int>(b);
                break;
            }
        }
        if (boneIndex < 0) continue;
        BoneTrack* track = nullptr;
        for (BoneTrack& t : clip.tracks) {
            if (t.boneIndex == boneIndex) { track = &t; break; }
        }
        if (!track) {
            clip.tracks.push_back(BoneTrack{ boneIndex, {} });
            track = &clip.tracks.back();
        }
        const size_t keyCount = channel.times.size();
        track->keyFrames.reserve(track->keyFrames.size() + keyCount);
        for (size_t i = 0; i < keyCount; ++i) {
            KeyFrame frame;
            frame.timeStamp = channel.times[i];
            if (i < channel.translations.size()) frame.position = channel.translations[i];
            if (i < channel.rotations.size()) frame.rotation = channel.rotations[i];
            if (i < channel.scales.size()) frame.scale = channel.scales[i];
            track->keyFrames.push_back(frame);
        }
    }
    return clip;
}

bool load_gltf_assets(const std::filesystem::path& source,
                      std::vector<GltfSkin>& skins,
                      std::vector<GltfAnimationClip>& clips,
                      std::string* error) {
    std::ifstream file(source, std::ios::binary);
    if (!file) {
        if (error) *error = "Cannot open glTF source";
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    std::string document;
    const std::string ext = source.extension().string();
    const std::string lowerExt = [&] {
        std::string e;
        for (char c : ext) e += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e;
    }();
    if (lowerExt == ".glb") {
        if (bytes.size() < 20 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "glTF") {
            if (error) *error = "Invalid GLB header";
            return false;
        }
        const auto readLE = [&](size_t offset) {
            return static_cast<uint32_t>(bytes[offset]) |
                   (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                   (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                   (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        };
        const uint32_t jsonLength = readLE(12);
        if (jsonLength == 0 || 20ull + jsonLength > bytes.size()) {
            if (error) *error = "Invalid GLB JSON chunk";
            return false;
        }
        document.assign(reinterpret_cast<const char*>(bytes.data() + 20), jsonLength);
        // BIN chunk (animation accessor payload): 4-byte aligned after JSON.
        std::vector<uint8_t> bin;
        size_t chunkOffset = 20 + ((jsonLength + 3) & ~3u);
        if (chunkOffset + 8 <= bytes.size()) {
            const uint32_t binLength = readLE(chunkOffset);
            const uint32_t binType = readLE(chunkOffset + 4);
            if (binType == 0x004E4942 && chunkOffset + 8 + binLength <= bytes.size()) {
                bin.assign(bytes.begin() + static_cast<std::ptrdiff_t>(chunkOffset + 8),
                           bytes.begin() + static_cast<std::ptrdiff_t>(chunkOffset + 8 + binLength));
            }
        }
        GltfParser parser(document, std::move(bin));
        if (!parser.parse(error)) return false;
        skins = parser.skins();
        clips = parser.clips();
        return true;
    }

    GltfParser parser(document);
    if (!parser.parse(error)) return false;
    skins = parser.skins();
    clips = parser.clips();
    return true;
}

// ─── GPU skinning ───
std::vector<glm::mat4> GpuSkinningBuffer::compute_bone_matrices(const SkeletonAsset& skeleton, const Pose& pose) {
    std::vector<glm::mat4> matrices(skeleton.bones.size(), glm::mat4(1.0f));
    for (int i = 0; i < static_cast<int>(skeleton.bones.size()); ++i) {
        glm::mat4 local = glm::mat4(1.0f);
        if (pose.local.empty() || i >= static_cast<int>(pose.local.size())) {
            local = skeleton.bones[i].localTransform;
        } else {
            const auto& t = pose.local[i];
            local = glm::translate(glm::mat4(1.0f), t.translation) *
                    glm::mat4_cast(t.rotation) *
                    glm::scale(glm::mat4(1.0f), t.scale);
        }
        // Accumulate parent chain.
        glm::mat4 world = local;
        int parent = skeleton.bones[i].parentIndex;
        while (parent >= 0 && parent < static_cast<int>(skeleton.bones.size())) {
            if (pose.local.empty() || parent >= static_cast<int>(pose.local.size())) {
                world = skeleton.bones[parent].localTransform * world;
            } else {
                const auto& t = pose.local[parent];
                world = glm::translate(glm::mat4(1.0f), t.translation) *
                        glm::mat4_cast(t.rotation) *
                        glm::scale(glm::mat4(1.0f), t.scale) * world;
            }
            parent = skeleton.bones[parent].parentIndex;
        }
        matrices[i] = world;
    }
    return matrices;
}

std::vector<float> GpuSkinningBuffer::pack(const std::vector<glm::mat4>& boneMatrices) {
    std::vector<float> out;
    out.reserve(boneMatrices.size() * 16);
    for (const glm::mat4& m : boneMatrices) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                out.push_back(m[c][r]);
    }
    return out;
}

std::string GpuSkinningBuffer::skinned_vertex_shader(uint32_t boneCount) {
    std::ostringstream out;
    out << "#version 450\n";
    out << "layout(location = 0) in vec3 inPosition;\n";
    out << "layout(location = 1) in vec3 inNormal;\n";
    out << "layout(location = 2) in vec2 inUv;\n";
    out << "layout(location = 3) in uvec4 inJoints;\n";
    out << "layout(location = 4) in vec4 inWeights;\n";
    out << "layout(location = 0) out vec2 vUv;\n";
    out << "layout(location = 1) out vec3 vWorldPos;\n";
    out << "layout(location = 2) out vec3 vNormal;\n";
    out << "layout(binding = 0) uniform BoneMatrices {\n";
    out << "    mat4 bones[" << boneCount << "];\n";
    out << "} bones;\n";
    out << "layout(binding = 1) uniform CameraData {\n";
    out << "    mat4 viewProj;\n";
    out << "} camera;\n";
    out << "void main() {\n";
    out << "    mat4 skin = inWeights.x * bones.bones[inJoints.x]\n";
    out << "             + inWeights.y * bones.bones[inJoints.y]\n";
    out << "             + inWeights.z * bones.bones[inJoints.z]\n";
    out << "             + inWeights.w * bones.bones[inJoints.w];\n";
    out << "    if (dot(inWeights, vec4(1.0)) < 1e-4) skin = mat4(1.0);\n";
    out << "    vec4 worldPos = skin * vec4(inPosition, 1.0);\n";
    out << "    gl_Position = camera.viewProj * worldPos;\n";
    out << "    vUv = inUv;\n";
    out << "    vWorldPos = worldPos.xyz;\n";
    out << "    vNormal = mat3(skin) * inNormal;\n";
    out << "}\n";
    return out.str();
}

} // namespace Engine
