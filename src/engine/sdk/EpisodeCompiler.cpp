// EpisodeCompiler.cpp — SDK adapter for the public IEpisodeCompiler contract
// (FALTANTES differential: validate, simulate, test, compress, sign and
// publish content — META §32). Single TU, pure, deterministic, all-or-nothing
// refusals. Parses JSON through the shared RegistryJson helpers; emission is
// a local deterministic emitter (std::map ordering, %.9g floats — the same
// convention as the other differential adapters).
#include "engine/compiler/IEpisodeCompiler.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace engine {
namespace compiler {
namespace {

constexpr int kManifestVersion = 1;
constexpr int kSimulationSteps = 16;  // deterministic dry-run length (fixed)

// Deterministic content digest (splitmix64 over the bytes; 16 hex chars).
// The default hasher — production wires BLAKE3 through the seam.
std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

std::string default_digest(const std::string& data) {
    std::uint64_t h = 0xE9E3779B97F4A7C1ULL;
    for (const char c : data) {
        h = splitmix64(h ^ static_cast<std::uint64_t>(
                               static_cast<unsigned char>(c)));
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                  static_cast<unsigned long long>(h),
                  static_cast<unsigned long long>(splitmix64(h)));
    return std::string(buffer);
}

std::string identity_compress(const std::string& data, std::string&) {
    return data;
}
std::string identity_decompress(const std::string& data, std::string&) {
    return data;
}

std::string escape_json(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

struct JsonValue {
    enum class Kind { Null, Str, Num, Bool, Arr, Obj } kind{ Kind::Null };
    std::string str;
    double num{ 0.0 };
    bool boolean{ false };
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    static JsonValue number(double value) {
        JsonValue v;
        v.kind = Kind::Num;
        v.num = value;
        return v;
    }
    static JsonValue text(const std::string& value) {
        JsonValue v;
        v.kind = Kind::Str;
        v.str = value;
        return v;
    }
    static JsonValue bool_value(bool value) {
        JsonValue v;
        v.kind = Kind::Bool;
        v.boolean = value;
        return v;
    }
    static JsonValue json_array(std::vector<JsonValue> values) {
        JsonValue v;
        v.kind = Kind::Arr;
        v.arr = std::move(values);
        return v;
    }
    static JsonValue json_object(std::map<std::string, JsonValue> fields) {
        JsonValue v;
        v.kind = Kind::Obj;
        v.obj = std::move(fields);
        return v;
    }
};

std::string emit_json(const JsonValue& value) {
    switch (value.kind) {
        case JsonValue::Kind::Null:
            return "null";
        case JsonValue::Kind::Bool:
            return value.boolean ? "true" : "false";
        case JsonValue::Kind::Num: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.9g", value.num);
            return std::string(buffer);
        }
        case JsonValue::Kind::Str:
            return "\"" + escape_json(value.str) + "\"";
        case JsonValue::Kind::Arr: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.arr.size(); ++i) {
                if (i != 0) out += ",";
                out += emit_json(value.arr[i]);
            }
            out += "]";
            return out;
        }
        case JsonValue::Kind::Obj: {
            std::string out = "{";
            bool first = true;
            for (const auto& entry : value.obj) {
                if (!first) out += ",";
                first = false;
                out += "\"" + escape_json(entry.first) + "\":" +
                       emit_json(entry.second);
            }
            out += "}";
            return out;
        }
    }
    return "null";
}

std::string uint64_str(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

bool parse_uint64(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

bool read_uint64_field(const engine::sdk::JsonValue& doc,
                       const std::string& key, std::uint64_t& out,
                       std::uint64_t defaultValue, std::string& errorOut) {
    const auto* field = doc.field(key);
    if (field == nullptr) {
        out = defaultValue;
        return true;
    }
    if (field->kind == engine::sdk::JsonValue::Kind::Number &&
        field->number >= 0.0) {
        out = static_cast<std::uint64_t>(field->number);
        return true;
    }
    if (field->is_string() && parse_uint64(field->string, out)) {
        return true;
    }
    errorOut = "field '" + key + "' must be a non-negative integer";
    return false;
}

// Deterministic length-prefixed packing: [u32 LE kind][kind][0x00][name][0x00]
// [json] per entry, in manifest order. All-or-nothing on oversized entries.
bool pack_entries(const std::vector<EpisodeEntry>& entries,
                  std::string& packedOut, std::string& errorOut) {
    std::string packed;
    for (const EpisodeEntry& entry : entries) {
        if (entry.kind.size() > 0xFFFFFFu || entry.name.size() > 0xFFFFFFu ||
            entry.json.size() > 0xFFFFFFu) {
            errorOut = "entry too large to pack: " + entry.name;
            return false;
        }
        const std::uint32_t kindSize =
            static_cast<std::uint32_t>(entry.kind.size());
        packed.push_back(static_cast<char>(kindSize & 0xFFu));
        packed.push_back(static_cast<char>((kindSize >> 8) & 0xFFu));
        packed.push_back(static_cast<char>((kindSize >> 16) & 0xFFu));
        packed.push_back(static_cast<char>((kindSize >> 24) & 0xFFu));
        packed += entry.kind;
        packed.push_back('\0');
        packed += entry.name;
        packed.push_back('\0');
        packed += entry.json;
    }
    packedOut = std::move(packed);
    return true;
}

class EpisodeCompiler final : public IEpisodeCompiler {
public:
    bool register_validator(const std::string& kind, EntryValidator validator,
                            std::string& errorOut) override {
        if (kind.empty()) {
            errorOut = "kind must be non-empty";
            return false;
        }
        if (!validator) {
            errorOut = "validator must not be null";
            return false;
        }
        validators_[kind] = std::move(validator);
        return true;
    }

    bool register_simulator(const std::string& kind, EntrySimulator simulator,
                            std::string& errorOut) override {
        if (kind.empty()) {
            errorOut = "kind must be non-empty";
            return false;
        }
        if (!simulator) {
            errorOut = "simulator must not be null";
            return false;
        }
        simulators_[kind] = std::move(simulator);
        return true;
    }

    bool register_tester(const std::string& kind, EntryTester tester,
                         std::string& errorOut) override {
        if (kind.empty()) {
            errorOut = "kind must be non-empty";
            return false;
        }
        if (!tester) {
            errorOut = "tester must not be null";
            return false;
        }
        testers_[kind] = std::move(tester);
        return true;
    }

    bool set_codec(CompressFn compress, DecompressFn decompress,
                   std::string& errorOut) override {
        if (!compress || !decompress) {
            errorOut = "codec hooks must not be null";
            return false;
        }
        compress_ = std::move(compress);
        decompress_ = std::move(decompress);
        return true;
    }

    bool set_hasher(HashFn hash, std::string& errorOut) override {
        if (!hash) {
            errorOut = "hasher must not be null";
            return false;
        }
        hasher_ = std::move(hash);
        return true;
    }

    bool compile(const EpisodeManifest& manifest, EpisodePackage& out,
                 std::string& errorOut) override {
        out = EpisodePackage{};
        // The pipeline is independent of the caller's prior diagnostic: a
        // successful compile leaves errorOut empty, and a stale non-empty
        // error must never poison a good compile (the codec seam reads it).
        errorOut.clear();
        if (manifest.version != kManifestVersion) {
            errorOut = "unsupported manifest version";
            return false;
        }
        if (manifest.title.empty()) {
            errorOut = "manifest title must be non-empty";
            return false;
        }
        std::set<std::string> names;
        for (const EpisodeEntry& entry : manifest.entries) {
            if (entry.name.empty()) {
                errorOut = "entry name must be non-empty";
                return false;
            }
            if (!names.insert(entry.name).second) {
                errorOut = "duplicate entry name: " + entry.name;
                return false;
            }
            if (entry.kind.empty()) {
                errorOut = "entry kind must be non-empty: " + entry.name;
                return false;
            }
        }

        // Stages 1-3: validate / simulate / test every entry (all-or-nothing).
        std::vector<EpisodeEntryResult> results;
        results.reserve(manifest.entries.size());
        for (const EpisodeEntry& entry : manifest.entries) {
            const auto validator = validators_.find(entry.kind);
            if (validator == validators_.end()) {
                errorOut = "no validator registered for kind: " + entry.kind;
                return false;
            }
            const auto simulator = simulators_.find(entry.kind);
            if (simulator == simulators_.end()) {
                errorOut = "no simulator registered for kind: " + entry.kind;
                return false;
            }
            const auto tester = testers_.find(entry.kind);
            if (tester == testers_.end()) {
                errorOut = "no tester registered for kind: " + entry.kind;
                return false;
            }
            std::string hookError;
            if (!validator->second(entry.json, hookError)) {
                errorOut = "validation failed for " + entry.name + ": " +
                           hookError;
                return false;
            }
            std::string trace;
            if (!simulator->second(entry.json, kSimulationSteps, trace,
                                   hookError)) {
                errorOut = "simulation failed for " + entry.name + ": " +
                           hookError;
                return false;
            }
            if (!tester->second(entry.json, hookError)) {
                errorOut = "test failed for " + entry.name + ": " + hookError;
                return false;
            }
            EpisodeEntryResult result;
            result.kind = entry.kind;
            result.name = entry.name;
            result.validated = true;
            result.simulated = true;
            result.tested = true;
            result.digest = hasher_(entry.json + "|" + trace);
            results.push_back(std::move(result));
        }

        // Stage 4-5: pack + compress (all-or-nothing).
        std::string packed;
        if (!pack_entries(manifest.entries, packed, errorOut)) return false;
        std::string payload;
        if (!(payload = compress_(packed, errorOut)).empty() ||
            !errorOut.empty()) {
            // The identity codec may legitimately produce an empty payload for
            // an empty episode; a NON-empty error still refuses.
            if (!errorOut.empty()) return false;
        }

        // Stage 6-7: canonical manifest + signature.
        const std::string manifestJson = build_manifest(manifest, results,
                                                        packed.size(),
                                                        payload.size());
        EpisodePackage result;
        result.manifestJson = manifestJson;
        result.payload = payload;
        result.signature = hasher_(manifestJson + payload);
        out = std::move(result);
        return true;
    }

    bool verify(const EpisodePackage& package,
                std::string& errorOut) const override {
        errorOut.clear();
        engine::sdk::JsonValue doc;
        if (!engine::sdk::json_parse(package.manifestJson, doc, errorOut)) {
            errorOut = "malformed package manifest: " + errorOut;
            return false;
        }
        std::uint64_t payloadLength = 0, compressedLength = 0;
        if (!read_uint64_field(doc, "payloadLength", payloadLength, 0,
                               errorOut) ||
            !read_uint64_field(doc, "payloadCompressedLength",
                               compressedLength, 0, errorOut)) {
            return false;
        }
        if (payloadLength > 0 && package.payload.empty() &&
            compressedLength != 0) {
            errorOut = "payload missing";
            return false;
        }
        if (package.payload.size() != compressedLength) {
            errorOut = "payload length mismatch";
            return false;
        }
        const std::string recomputed = hasher_(package.manifestJson +
                                               package.payload);
        if (recomputed != package.signature) {
            errorOut = "signature mismatch";
            return false;
        }
        std::string content;
        if (!(content = decompress_(package.payload, errorOut)).empty() ||
            !errorOut.empty()) {
            if (!errorOut.empty()) return false;
        }
        if (content.size() != payloadLength) {
            errorOut = "payload content length mismatch";
            return false;
        }
        return true;
    }

    bool unpack(const EpisodePackage& package, std::string& contentOut,
                std::string& errorOut) const override {
        errorOut.clear();
        contentOut = decompress_(package.payload, errorOut);
        if (!errorOut.empty()) return false;
        return true;
    }

private:
    std::string build_manifest(const EpisodeManifest& manifest,
                               const std::vector<EpisodeEntryResult>& results,
                               std::size_t payloadLength,
                               std::size_t compressedLength) const {
        std::vector<JsonValue> entries;
        for (const EpisodeEntryResult& result : results) {
            std::map<std::string, JsonValue> fields;
            fields["kind"] = JsonValue::text(result.kind);
            fields["name"] = JsonValue::text(result.name);
            fields["validated"] = JsonValue::bool_value(result.validated);
            fields["simulated"] = JsonValue::bool_value(result.simulated);
            fields["tested"] = JsonValue::bool_value(result.tested);
            fields["digest"] = JsonValue::text(result.digest);
            entries.push_back(JsonValue::json_object(std::move(fields)));
        }
        std::map<std::string, JsonValue> doc;
        doc["version"] = JsonValue::number(kManifestVersion);
        doc["title"] = JsonValue::text(manifest.title);
        doc["stepCount"] = JsonValue::number(kSimulationSteps);
        doc["payloadLength"] =
            JsonValue::text(uint64_str(payloadLength));
        doc["payloadCompressedLength"] =
            JsonValue::text(uint64_str(compressedLength));
        doc["entries"] = JsonValue::json_array(std::move(entries));
        return emit_json(JsonValue::json_object(std::move(doc)));
    }

    std::map<std::string, EntryValidator> validators_;
    std::map<std::string, EntrySimulator> simulators_;
    std::map<std::string, EntryTester> testers_;
    CompressFn compress_{ identity_compress };
    DecompressFn decompress_{ identity_decompress };
    HashFn hasher_{ default_digest };
};

}  // namespace

std::unique_ptr<IEpisodeCompiler> create_episode_compiler() {
    return std::unique_ptr<IEpisodeCompiler>(new EpisodeCompiler());
}

}  // namespace compiler
}  // namespace engine
