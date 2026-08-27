// JsonSchema.cpp — SDK adapter for engine/ui/IJsonSchema.hpp (agente 2,
// item `glaze`). The ONLY TU with behavior. Validates JSON documents against
// declared schemas using the ENGINE'S SHARED JSON PARSER (JsonMini — the
// consolidation authority for authoring JSON), all-or-nothing:
//   - required fields must be present;
//   - field types must match (Bool/Number/String/Array);
//   - Number fields must fall within the declared range;
//   - strict mode rejects unknown fields;
//   - malformed JSON is refused.
//
// This is the schema-validation core served by glaze's schema generation,
// implemented on the shared parser so authoring configs validate without a
// glaze dependency.

#include "engine/ui/IJsonSchema.hpp"

#include "engine/core/serialization/JsonMini.hpp"

#include <string>

namespace engine {
namespace ui {

class JsonSchema final : public IJsonSchema {
public:
    bool validate(const JsonSchemaSpec& schema, const std::string& jsonText,
                  std::string& errorOut) override {
        // Parse with the SHARED parser (single authority).
        std::string parseErr;
        const Engine::Json::Value root = Engine::Json::parse(jsonText, &parseErr);
        if (root.is_null() && !jsonText.empty() && !parseErr.empty()) {
            errorOut = "json schema [" + schema.name + "]: " + parseErr;
            return false;
        }
        if (!root.is_object()) {
            errorOut = "json schema [" + schema.name +
                       "]: document root must be an object";
            return false;
        }
        // Required fields present + type/range checks + strict unknown check.
        for (const SchemaField& f : schema.fields) {
            const Engine::Json::Value* v = root.find(f.name);
            if (v == nullptr) {
                if (f.required) {
                    errorOut = "json schema [" + schema.name +
                               "]: missing required field '" + f.name + "'";
                    return false;
                }
                continue;
            }
            switch (f.type) {
            case SchemaFieldType::Bool:
                if (!v->is_bool()) {
                    errorOut = "json schema [" + schema.name +
                               "]: field '" + f.name + "' must be bool";
                    return false;
                }
                break;
            case SchemaFieldType::Number:
                if (!v->is_number()) {
                    errorOut = "json schema [" + schema.name +
                               "]: field '" + f.name + "' must be a number";
                    return false;
                }
                if (f.hasRange) {
                    const double n = v->as_number();
                    if (n < f.min || n > f.max) {
                        errorOut = "json schema [" + schema.name +
                                   "]: field '" + f.name + "' out of range [" +
                                   std::to_string(f.min) + ", " +
                                   std::to_string(f.max) + "]";
                        return false;
                    }
                }
                break;
            case SchemaFieldType::String:
                if (!v->is_string()) {
                    errorOut = "json schema [" + schema.name +
                               "]: field '" + f.name + "' must be a string";
                    return false;
                }
                break;
            case SchemaFieldType::Array:
                if (!v->is_array()) {
                    errorOut = "json schema [" + schema.name +
                               "]: field '" + f.name + "' must be an array";
                    return false;
                }
                break;
            }
        }
        // Strict mode: no unknown fields.
        if (schema.strict) {
            for (const auto& kv : root.object()) {
                bool known = false;
                for (const SchemaField& f : schema.fields) {
                    if (f.name == kv.first) { known = true; break; }
                }
                if (!known) {
                    errorOut = "json schema [" + schema.name +
                               "]: unknown field '" + kv.first + "' (strict)";
                    return false;
                }
            }
        }
        return true;
    }
};

}  // namespace ui

namespace ui {

std::shared_ptr<IJsonSchema> create_json_schema() {
    // Declared in engine/ui/IJsonSchema.hpp as engine::ui::create_json_schema;
    // JsonSchema lives in engine::ui, so the factory must stay in that
    // namespace (its declaration does) for make_shared<JsonSchema> to resolve.
    return std::make_shared<JsonSchema>();
}

}  // namespace ui

}  // namespace engine
