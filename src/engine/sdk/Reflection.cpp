// Reflection.cpp — the only TU implementing the public reflection registry
// (Agente 4 §1 item 50 CORE): a schema registry of types and fields for
// editor/scripting/MCP. Pure std + RegistryJson; deterministic order.

#include "engine/entity/IReflection.hpp"

#include "RegistryJson.hpp"

#include <map>
#include <sstream>
#include <vector>

namespace engine {
namespace entity {

const char* field_kind_name(FieldKind kind) {
    switch (kind) {
        case FieldKind::Int: return "int";
        case FieldKind::Float: return "float";
        case FieldKind::Bool: return "bool";
        case FieldKind::String: return "string";
        case FieldKind::Vec3: return "vec3";
        case FieldKind::Quat: return "quat";
        case FieldKind::Enum: return "enum";
        case FieldKind::Json: return "json";
    }
    return "float";
}

namespace {

bool parse_kind(const std::string& text, FieldKind& out) {
    if (text == "int") out = FieldKind::Int;
    else if (text == "float") out = FieldKind::Float;
    else if (text == "bool") out = FieldKind::Bool;
    else if (text == "string") out = FieldKind::String;
    else if (text == "vec3") out = FieldKind::Vec3;
    else if (text == "quat") out = FieldKind::Quat;
    else if (text == "enum") out = FieldKind::Enum;
    else if (text == "json") out = FieldKind::Json;
    else return false;
    return true;
}

bool check_type(const TypeInfo& type, std::string& errorOut) {
    if (type.name.empty()) {
        errorOut = "reflection: type name must be non-empty";
        return false;
    }
    for (std::size_t i = 0; i < type.fields.size(); ++i) {
        if (type.fields[i].name.empty()) {
            errorOut = "reflection: type '" + type.name +
                       "' has a field with empty name";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (type.fields[j].name == type.fields[i].name) {
                errorOut = "reflection: type '" + type.name +
                           "' has duplicate field '" + type.fields[i].name + "'";
                return false;
            }
        }
    }
    return true;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

class Reflection final : public IReflection {
public:
    Reflection() = default;

    bool register_type(const TypeInfo& type, std::string& errorOut) override {
        if (!check_type(type, errorOut)) return false;
        if (types_.count(type.name) != 0) {
            errorOut = "reflection: duplicate type '" + type.name + "'";
            return false;
        }
        types_[type.name] = type;
        return true;
    }

    bool load_from_json(const std::string& jsonText,
                        std::string& errorOut) override {
        sdk::JsonValue root;
        if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
            if (errorOut.empty()) errorOut = "reflection: root must be an object";
            return false;
        }
        const int version = static_cast<int>(sdk::json_number(root, "version", 1));
        if (version != 1) {
            errorOut = "reflection: unsupported version " + std::to_string(version);
            return false;
        }
        const sdk::JsonValue* listValue = root.field("types");
        if (listValue == nullptr || listValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "reflection: types must be an array";
            return false;
        }
        std::map<std::string, TypeInfo> parsed;
        for (const sdk::JsonValue& typeValue : listValue->array) {
            if (!typeValue.is_object()) {
                errorOut = "reflection: each type must be an object";
                return false;
            }
            TypeInfo type;
            type.name = sdk::json_string(typeValue, "name", "");
            const sdk::JsonValue* fieldsValue = typeValue.field("fields");
            if (fieldsValue != nullptr) {
                if (fieldsValue->kind != sdk::JsonValue::Kind::Array) {
                    errorOut = "reflection: fields must be an array";
                    return false;
                }
                for (const sdk::JsonValue& fieldValue : fieldsValue->array) {
                    if (!fieldValue.is_object()) {
                        errorOut = "reflection: each field must be an object";
                        return false;
                    }
                    FieldInfo field;
                    field.name = sdk::json_string(fieldValue, "name", "");
                    const std::string kindName =
                        sdk::json_string(fieldValue, "kind", "float");
                    if (!parse_kind(kindName, field.kind)) {
                        errorOut = "reflection: unknown field kind '" + kindName + "'";
                        return false;
                    }
                    type.fields.push_back(std::move(field));
                }
            }
            if (!check_type(type, errorOut)) return false;
            if (parsed.count(type.name) != 0) {
                errorOut = "reflection: duplicate type '" + type.name + "'";
                return false;
            }
            parsed[type.name] = std::move(type);
        }
        types_ = std::move(parsed);
        return true;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"version\":1,\"types\":[";
        bool first = true;
        for (const auto& entry : types_) {  // map: ordem crescente
            if (!first) out << ",";
            first = false;
            const TypeInfo& type = entry.second;
            out << "{\"name\":\"" << json_escape(type.name) << "\",\"fields\":[";
            for (std::size_t i = 0; i < type.fields.size(); ++i) {
                if (i != 0) out << ",";
                out << "{\"name\":\"" << json_escape(type.fields[i].name)
                    << "\",\"kind\":\"" << field_kind_name(type.fields[i].kind)
                    << "\"}";
            }
            out << "]}";
        }
        out << "]}";
        return out.str();
    }

    const TypeInfo* type(const std::string& name) const override {
        const auto found = types_.find(name);
        return found == types_.end() ? nullptr : &found->second;
    }

    std::vector<std::string> type_names() const override {
        std::vector<std::string> out;
        out.reserve(types_.size());
        for (const auto& entry : types_) out.push_back(entry.first);
        return out;
    }

    std::vector<std::string> field_names(const std::string& typeName) const override {
        std::vector<std::string> out;
        const auto found = types_.find(typeName);
        if (found == types_.end()) return out;
        for (const FieldInfo& field : found->second.fields) out.push_back(field.name);
        return out;
    }

    bool has_field(const std::string& typeName, const std::string& field) const override {
        const auto found = types_.find(typeName);
        if (found == types_.end()) return false;
        for (const FieldInfo& info : found->second.fields) {
            if (info.name == field) return true;
        }
        return false;
    }

    std::size_t count() const override { return types_.size(); }
    void clear() override { types_.clear(); }

private:
    std::map<std::string, TypeInfo> types_;
};

}  // namespace

std::unique_ptr<IReflection> create_reflection() {
    return std::make_unique<Reflection>();
}

}  // namespace entity
}  // namespace engine
