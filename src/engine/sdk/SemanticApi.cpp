// SemanticApi.cpp — SDK adapter for the public ISemanticApi contract
// (FALTANTES differential: ONE surface for C++, editor, scripting, CLI and
// MCP — the deterministic kind-agnostic semantic core: schema per kind +
// all-or-nothing validation + bit-exact canonicalization). Single TU, pure,
// deterministic. Parses JSON through the shared RegistryJson helpers;
// emission is a local deterministic emitter (schema-order keys, %.9g floats
// — the same convention as the other differential adapters).
#include "engine/semantic/ISemanticApi.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
namespace semantic {
namespace {

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

std::string double_str(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return std::string(buffer);
}

std::string int_str(long long value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", value);
    return std::string(buffer);
}

std::string str_frag(const std::string& text) {
    return "\"" + escape_json(text) + "\"";
}

std::string bool_frag(bool value) { return value ? "true" : "false"; }

std::string array_frag(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ",";
        out += str_frag(values[i]);
    }
    out += "]";
    return out;
}

std::string type_name(SemanticFieldType type) {
    switch (type) {
        case SemanticFieldType::String:
            return "string";
        case SemanticFieldType::Int:
            return "int";
        case SemanticFieldType::Double:
            return "double";
        case SemanticFieldType::Bool:
            return "bool";
        case SemanticFieldType::StringArray:
            return "stringArray";
    }
    return "string";
}

bool parse_type(const std::string& text, SemanticFieldType& out) {
    if (text == "string") {
        out = SemanticFieldType::String;
        return true;
    }
    if (text == "int") {
        out = SemanticFieldType::Int;
        return true;
    }
    if (text == "double") {
        out = SemanticFieldType::Double;
        return true;
    }
    if (text == "bool") {
        out = SemanticFieldType::Bool;
        return true;
    }
    if (text == "stringArray") {
        out = SemanticFieldType::StringArray;
        return true;
    }
    return false;
}

// Reads a double field (number or numeric string) — the RegistryJson
// convention used by the other adapters.
bool read_double_field(const engine::sdk::JsonValue& object,
                       const std::string& key, double& out, bool& present,
                       std::string& errorOut) {
    const auto* field = object.field(key);
    if (field == nullptr) {
        present = false;
        return true;
    }
    present = true;
    if (field->kind == engine::sdk::JsonValue::Kind::Number) {
        out = field->number;
        return true;
    }
    if (field->is_string()) {
        char* end = nullptr;
        const double value = std::strtod(field->string.c_str(), &end);
        if (end != nullptr && *end == '\0' && end != field->string.c_str()) {
            out = value;
            return true;
        }
    }
    errorOut = "field '" + key + "' must be a number";
    return false;
}

bool validate_field_schema(const SemanticField& field, std::string& errorOut) {
    if (field.name.empty()) {
        errorOut = "field name must be non-empty";
        return false;
    }
    if (field.type > SemanticFieldType::StringArray) {
        errorOut = "field '" + field.name + "' has an unknown type";
        return false;
    }
    if (field.hasMin && field.hasMax && field.min > field.max) {
        errorOut = "field '" + field.name + "' has min > max";
        return false;
    }
    if (!field.allowed.empty() && field.type != SemanticFieldType::String) {
        errorOut = "field '" + field.name +
                   "': allowed values only apply to string fields";
        return false;
    }
    if (field.hasDefault) {
        if (field.type == SemanticFieldType::Int &&
            field.defaultNumber != std::floor(field.defaultNumber)) {
            errorOut = "field '" + field.name +
                       "': int default must be integral";
            return false;
        }
        if ((field.type == SemanticFieldType::Int ||
             field.type == SemanticFieldType::Double) &&
            !std::isfinite(field.defaultNumber)) {
            errorOut = "field '" + field.name + "': default must be finite";
            return false;
        }
    }
    return true;
}

// The canonical JSON fragment for a field VALUE (given + default).
std::string field_value_frag(const SemanticField& field,
                             const engine::sdk::JsonValue& value,
                             std::string& errorOut) {
    switch (field.type) {
        case SemanticFieldType::String: {
            if (!value.is_string()) {
                errorOut = "field '" + field.name + "' must be a string";
                return "";
            }
            if (!field.allowed.empty()) {
                bool ok = false;
                for (const std::string& allowed : field.allowed) {
                    if (allowed == value.string) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    errorOut = "field '" + field.name + "' value '" +
                               value.string + "' is not allowed";
                    return "";
                }
            }
            return str_frag(value.string);
        }
        case SemanticFieldType::Int: {
            if (value.kind != engine::sdk::JsonValue::Kind::Number ||
                value.number != std::floor(value.number) ||
                !std::isfinite(value.number)) {
                errorOut = "field '" + field.name +
                           "' must be an integral number";
                return "";
            }
            if (field.hasMin && value.number < field.min) {
                errorOut = "field '" + field.name + "' below minimum " +
                           double_str(field.min);
                return "";
            }
            if (field.hasMax && value.number > field.max) {
                errorOut = "field '" + field.name + "' above maximum " +
                           double_str(field.max);
                return "";
            }
            return int_str(static_cast<long long>(value.number));
        }
        case SemanticFieldType::Double: {
            if (value.kind != engine::sdk::JsonValue::Kind::Number ||
                !std::isfinite(value.number)) {
                errorOut = "field '" + field.name +
                           "' must be a finite number";
                return "";
            }
            if (field.hasMin && value.number < field.min) {
                errorOut = "field '" + field.name + "' below minimum " +
                           double_str(field.min);
                return "";
            }
            if (field.hasMax && value.number > field.max) {
                errorOut = "field '" + field.name + "' above maximum " +
                           double_str(field.max);
                return "";
            }
            return double_str(value.number);
        }
        case SemanticFieldType::Bool: {
            if (value.kind != engine::sdk::JsonValue::Kind::Bool) {
                errorOut = "field '" + field.name + "' must be a boolean";
                return "";
            }
            return bool_frag(value.boolean);
        }
        case SemanticFieldType::StringArray: {
            if (!value.is_array()) {
                errorOut = "field '" + field.name +
                           "' must be an array of strings";
                return "";
            }
            std::vector<std::string> values;
            for (const engine::sdk::JsonValue& element : value.array) {
                if (!element.is_string()) {
                    errorOut = "field '" + field.name +
                               "' must be an array of strings";
                    return "";
                }
                values.push_back(element.string);
            }
            return array_frag(values);
        }
    }
    errorOut = "field '" + field.name + "': unhandled type";
    return "";
}

std::string field_default_frag(const SemanticField& field) {
    switch (field.type) {
        case SemanticFieldType::String:
            return str_frag(field.defaultString);
        case SemanticFieldType::Int:
            return int_str(
                static_cast<long long>(field.defaultNumber));
        case SemanticFieldType::Double:
            return double_str(field.defaultNumber);
        case SemanticFieldType::Bool:
            return bool_frag(field.defaultBool);
        case SemanticFieldType::StringArray:
            return array_frag(field.defaultStrings);
    }
    return "null";
}

class SemanticApi final : public ISemanticApi {
public:
    bool register_kind(const SemanticKind& kind,
                       std::string& errorOut) override {
        // Successful operations leave the diagnostic empty; a stale
        // caller error must never leak into a later check (EpisodeCompiler
        // lesson, applied proactively).
        errorOut.clear();
        if (kind.name.empty()) {
            errorOut = "kind name must be non-empty";
            return false;
        }
        if (kindIndex_.find(kind.name) != kindIndex_.end()) {
            errorOut = "kind already registered: " + kind.name;
            return false;
        }
        if (kind.version < 1) {
            errorOut = "kind '" + kind.name + "' must have version >= 1";
            return false;
        }
        if (kind.fields.empty()) {
            errorOut = "kind '" + kind.name +
                       "' must declare at least one field";
            return false;
        }
        std::set<std::string> names;
        for (const SemanticField& field : kind.fields) {
            if (!validate_field_schema(field, errorOut)) {
                errorOut = "kind '" + kind.name + "': " + errorOut;
                return false;
            }
            if (!names.insert(field.name).second) {
                errorOut = "kind '" + kind.name +
                           "' has a duplicate field: " + field.name;
                return false;
            }
        }
        kindIndex_[kind.name] = kinds_.size();
        kinds_.push_back(kind);
        return true;
    }

    bool register_kind_json(const std::string& schemaJson,
                            std::string& errorOut) override {
        errorOut.clear();
        engine::sdk::JsonValue doc;
        if (!engine::sdk::json_parse(schemaJson, doc, errorOut)) {
            errorOut = "malformed schema: " + errorOut;
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "schema must be a JSON object";
            return false;
        }
        const auto* nameValue = doc.field("name");
        if (nameValue == nullptr || !nameValue->is_string() ||
            nameValue->string.empty()) {
            errorOut = "schema 'name' must be a non-empty string";
            return false;
        }
        const auto* versionValue = doc.field("version");
        int version = 1;
        if (versionValue != nullptr) {
            if (versionValue->kind !=
                    engine::sdk::JsonValue::Kind::Number ||
                versionValue->number != std::floor(versionValue->number)) {
                errorOut = "schema 'version' must be an integral number";
                return false;
            }
            version = static_cast<int>(versionValue->number);
        }
        const auto* fieldsValue = doc.field("fields");
        if (fieldsValue == nullptr || !fieldsValue->is_array()) {
            errorOut = "schema 'fields' must be an array";
            return false;
        }

        SemanticKind kind;
        kind.name = nameValue->string;
        kind.version = version;
        for (const engine::sdk::JsonValue& fieldDoc : fieldsValue->array) {
            SemanticField field;
            if (!parse_field(fieldDoc, field, errorOut)) return false;
            kind.fields.push_back(std::move(field));
        }
        return register_kind(kind, errorOut);
    }

    std::vector<std::string> kinds() const override {
        std::vector<std::string> names;
        names.reserve(kinds_.size());
        for (const SemanticKind& kind : kinds_) {
            names.push_back(kind.name);
        }
        return names;
    }

    const SemanticKind* kind(const std::string& name) const override {
        const auto found = kindIndex_.find(name);
        if (found == kindIndex_.end()) return nullptr;
        return &kinds_[found->second];
    }

    std::string kind_schema_to_json(const std::string& name) const override {
        const SemanticKind* found = kind(name);
        if (found == nullptr) return "";
        return emit_kind(*found);
    }

    std::string schema_to_json() const override {
        std::string out = "{\"kinds\":[";
        for (std::size_t i = 0; i < kinds_.size(); ++i) {
            if (i != 0) out += ",";
            out += emit_kind(kinds_[i]);
        }
        out += "]}";
        return out;
    }

    bool validate(const std::string& kindName, const std::string& document,
                  std::string& canonicalOut, std::string& errorOut) override {
        errorOut.clear();
        canonicalOut.clear();
        const SemanticKind* found = kind(kindName);
        if (found == nullptr) {
            errorOut = "unknown semantic kind: " + kindName;
            return false;
        }
        engine::sdk::JsonValue doc;
        if (!engine::sdk::json_parse(document, doc, errorOut)) {
            errorOut = "malformed document: " + errorOut;
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "semantic document must be a JSON object";
            return false;
        }

        // Unknown document keys are refused (the surface never guesses).
        std::set<std::string> declared;
        for (const SemanticField& field : found->fields) {
            declared.insert(field.name);
        }
        for (const auto& entry : doc.object) {
            if (declared.find(entry.first) == declared.end()) {
                errorOut = "unknown field '" + entry.first + "'";
                return false;
            }
        }

        // Canonical document: keys in schema declaration order, defaults
        // applied for missing optional fields, %.9g numbers.
        std::string canonical = "{";
        bool first = true;
        for (const SemanticField& field : found->fields) {
            const engine::sdk::JsonValue* value = doc.field(field.name);
            std::string fragment;
            if (value != nullptr) {
                fragment = field_value_frag(field, *value, errorOut);
                if (!errorOut.empty()) {
                    errorOut = "kind '" + kindName + "': " + errorOut;
                    return false;
                }
            } else if (field.required) {
                errorOut = "kind '" + kindName +
                           "': missing required field '" + field.name + "'";
                return false;
            } else if (field.hasDefault) {
                fragment = field_default_frag(field);
            } else {
                continue;
            }
            if (!first) canonical += ",";
            first = false;
            canonical += str_frag(field.name);
            canonical += ":";
            canonical += fragment;
        }
        canonical += "}";
        canonicalOut = std::move(canonical);
        return true;
    }

private:
    std::string emit_kind(const SemanticKind& kind) const {
        std::string out = "{\"name\":";
        out += str_frag(kind.name);
        out += ",\"version\":";
        out += int_str(kind.version);
        out += ",\"fields\":[";
        for (std::size_t i = 0; i < kind.fields.size(); ++i) {
            if (i != 0) out += ",";
            out += emit_field(kind.fields[i]);
        }
        out += "]}";
        return out;
    }

    std::string emit_field(const SemanticField& field) const {
        std::string out = "{\"name\":";
        out += str_frag(field.name);
        out += ",\"type\":";
        out += str_frag(type_name(field.type));
        out += ",\"required\":";
        out += bool_frag(field.required);
        if (field.hasDefault) {
            out += ",\"default\":";
            out += field_default_frag(field);
        }
        if (field.hasMin) {
            out += ",\"min\":";
            out += double_str(field.min);
        }
        if (field.hasMax) {
            out += ",\"max\":";
            out += double_str(field.max);
        }
        if (!field.allowed.empty()) {
            out += ",\"allowed\":";
            out += array_frag(field.allowed);
        }
        out += "}";
        return out;
    }

    // Parses one field document of a kind schema (all-or-nothing).
    bool parse_field(const engine::sdk::JsonValue& fieldDoc,
                     SemanticField& field, std::string& errorOut) {
        if (!fieldDoc.is_object()) {
            errorOut = "schema field must be a JSON object";
            return false;
        }
        const auto* nameValue = fieldDoc.field("name");
        if (nameValue == nullptr || !nameValue->is_string() ||
            nameValue->string.empty()) {
            errorOut = "schema field 'name' must be a non-empty string";
            return false;
        }
        field.name = nameValue->string;

        const auto* typeValue = fieldDoc.field("type");
        if (typeValue == nullptr || !typeValue->is_string() ||
            !parse_type(typeValue->string, field.type)) {
            errorOut = "schema field '" + field.name +
                       "' has an unknown type";
            return false;
        }

        const auto* requiredValue = fieldDoc.field("required");
        if (requiredValue != nullptr) {
            if (requiredValue->kind != engine::sdk::JsonValue::Kind::Bool) {
                errorOut = "schema field '" + field.name +
                           "' 'required' must be a boolean";
                return false;
            }
            field.required = requiredValue->boolean;
        }

        const auto* defaultValue = fieldDoc.field("default");
        if (defaultValue != nullptr) {
            field.hasDefault = true;
            switch (field.type) {
                case SemanticFieldType::String:
                    if (!defaultValue->is_string()) {
                        errorOut = "schema field '" + field.name +
                                   "' default must be a string";
                        return false;
                    }
                    field.defaultString = defaultValue->string;
                    break;
                case SemanticFieldType::Int:
                case SemanticFieldType::Double:
                    if (defaultValue->kind !=
                            engine::sdk::JsonValue::Kind::Number ||
                        !std::isfinite(defaultValue->number)) {
                        errorOut = "schema field '" + field.name +
                                   "' default must be a finite number";
                        return false;
                    }
                    field.defaultNumber = defaultValue->number;
                    break;
                case SemanticFieldType::Bool:
                    if (defaultValue->kind !=
                        engine::sdk::JsonValue::Kind::Bool) {
                        errorOut = "schema field '" + field.name +
                                   "' default must be a boolean";
                        return false;
                    }
                    field.defaultBool = defaultValue->boolean;
                    break;
                case SemanticFieldType::StringArray: {
                    if (!defaultValue->is_array()) {
                        errorOut = "schema field '" + field.name +
                                   "' default must be an array of strings";
                        return false;
                    }
                    for (const engine::sdk::JsonValue& element :
                         defaultValue->array) {
                        if (!element.is_string()) {
                            errorOut = "schema field '" + field.name +
                                       "' default must be an array of strings";
                            return false;
                        }
                        field.defaultStrings.push_back(element.string);
                    }
                    break;
                }
            }
        }

        double minValue = 0.0, maxValue = 0.0;
        bool minPresent = false, maxPresent = false;
        if (field.type == SemanticFieldType::Int ||
            field.type == SemanticFieldType::Double) {
            if (!read_double_field(fieldDoc, "min", minValue, minPresent,
                                   errorOut) ||
                !read_double_field(fieldDoc, "max", maxValue, maxPresent,
                                   errorOut)) {
                errorOut = "schema field '" + field.name + "': " + errorOut;
                return false;
            }
            field.hasMin = minPresent;
            field.min = minValue;
            field.hasMax = maxPresent;
            field.max = maxValue;
        }

        const auto* allowedValue = fieldDoc.field("allowed");
        if (allowedValue != nullptr) {
            if (!allowedValue->is_array()) {
                errorOut = "schema field '" + field.name +
                           "' 'allowed' must be an array of strings";
                return false;
            }
            for (const engine::sdk::JsonValue& element : allowedValue->array) {
                if (!element.is_string()) {
                    errorOut = "schema field '" + field.name +
                               "' 'allowed' must be an array of strings";
                    return false;
                }
                field.allowed.push_back(element.string);
            }
        }
        return true;
    }

    std::vector<SemanticKind> kinds_;
    std::unordered_map<std::string, std::size_t> kindIndex_;
};

}  // namespace

std::unique_ptr<ISemanticApi> create_semantic_api() {
    return std::unique_ptr<ISemanticApi>(new SemanticApi());
}

}  // namespace semantic
}  // namespace engine
