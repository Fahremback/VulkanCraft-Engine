#pragma once

// IJsonSchema (agente 2 — item `glaze`): PUBLIC JSON-schema contract — the
// \"generate schemas\" half of the glaze item. Declares the expected shape of
// a JSON document (required fields, per-field type + range) and validates
// documents against it, all-or-nothing, deterministic and headless.
//
// Design (deterministic, pure):
//   - A schema is a list of field declarations: name, type (Bool / Number /
//     String / Array), required flag, and optional numeric range [min, max]
//     for Number fields.
//   - Validation parses the document with the ENGINE'S SINGLE shared JSON
//     parser (Engine::Json::JsonMini — the consolidation target) and checks:
//     required fields present; field types match; Number fields within the
//     declared range; no unknown fields (strict) when strict=true.
//   - All-or-nothing: a document violating any rule is refused with a
//     message naming the first violation. Deterministic and bit-exact.
//
// This is the schema-authoring core (JSON Schema subset) served by glaze's
// schema generation — implemented on the shared JsonMini parser so authoring
// configs can be validated without a glaze dependency. Self-contained (std +
// Engine::Json). The SDK adapter (src/engine/sdk/JsonSchema.cpp) is the ONLY
// TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace ui {

enum class SchemaFieldType : std::uint8_t { Bool, Number, String, Array };

struct SchemaField {
    std::string name;
    SchemaFieldType type{ SchemaFieldType::Number };
    bool required{ false };
    double min{ 0.0 };   // Number range (inclusive); ignored for other types
    double max{ 0.0 };
    bool hasRange{ false };
};

struct JsonSchemaSpec {
    std::string name;               // schema id (for error messages)
    std::vector<SchemaField> fields; // field declarations
    bool strict{ true };            // reject unknown fields when true
};

class IJsonSchema {
public:
    virtual ~IJsonSchema() = default;

    // Validates a JSON document against a schema. All-or-nothing: returns
    // false with a message on the first violation (missing required field,
    // type mismatch, out-of-range number, unknown field in strict mode,
    // malformed JSON). Deterministic.
    virtual bool validate(const JsonSchemaSpec& schema,
                          const std::string& jsonText,
                          std::string& errorOut) = 0;
};

// Factory (implemented by the SDK adapter — the only TU with behavior).
std::shared_ptr<IJsonSchema> create_json_schema();

}  // namespace ui
}  // namespace engine
