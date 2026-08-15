#pragma once

// Internal helpers for the public registries: a small self-contained JSON
// parser (the engine has no external JSON dependency) and deterministic UUID
// derivation. These are NOT part of the public SDK surface.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace engine {
namespace sdk {

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind{ Kind::Null };
    bool boolean{ false };
    double number{ 0.0 };
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_string() const { return kind == Kind::String; }

    const JsonValue* field(const std::string& key) const {
        const auto found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }
};

// Parses a JSON document; returns false with a diagnostic (line/column) on
// malformed input.
bool json_parse(const std::string& text, JsonValue& out, std::string& errorOut);

// Reads a string field; returns defaultValue when missing/null/non-string.
std::string json_string(const JsonValue& object, const std::string& key,
                        const std::string& defaultValue);
double json_number(const JsonValue& object, const std::string& key, double defaultValue);
bool json_bool(const JsonValue& object, const std::string& key, bool defaultValue);
std::vector<std::string> json_string_array(const JsonValue& object, const std::string& key);
// Reads a numeric array field (e.g. "color": [1.0, 0.2, 0.2]); entries that
// are not numbers are skipped.
std::vector<double> json_number_array(const JsonValue& object, const std::string& key);

// Deterministic 128-bit UUID derived from a namespaced name (FNV-1a over two
// seeds). The same name always yields the same canonical UUID, on every
// platform, so ids are stable across runs without a random generator.
std::string stable_uuid(const std::string& namespacedName);

// Returns `uuid` when it is a canonical UUID, otherwise derives one from the
// namespaced name. Used so assets may either pin an explicit id or rely on a
// stable derived id.
std::string uuid_or_derived(const std::string& uuid, const std::string& namespacedName);

}  // namespace sdk
}  // namespace engine
