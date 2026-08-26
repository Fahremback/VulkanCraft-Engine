#pragma once

// ISemanticApi — FALTANTES differential "SemanticEngineAPI": ONE surface for
// C++, editor, scripting, CLI and MCP (META §32 — engine-own code; nothing in
// external/solutions resolves it).
//
// The engine's content (registry entries, materials, physics materials,
// scenes, missions, abilities...) is addressed by MANY hosts: the C++
// contracts parse documents, the MCP authoring tools mirror them, the CLI
// validates them, the editor edits them. Without a shared core, every host
// re-implements the document shape and the validation rules — and they drift
// (the "never guessed" rule: a host must never guess a shape the runtime
// rejects). This differential is that shared core: a KIND-AGNOSTIC semantic
// registry where each content kind declares its SCHEMA once
// (fields: name, type, required, default, min/max, allowed values) and every
// host validates and CANONICALIZES documents through the SAME deterministic
// surface.
//
// Semantics (deterministic, all-or-nothing):
//   - A document is validated against the kind's schema: every declared field
//     is type-checked and constraint-checked (min/max for numbers, allowed
//     for strings, integrality for ints, finite for doubles); an UNKNOWN
//     document key is refused (the surface never guesses); a MISSING REQUIRED
//     field is refused with the field name.
//   - validate() returns the CANONICAL document: keys in schema declaration
//     order, defaults applied for missing optional fields, numbers emitted
//     %.9g, integral ints emitted without ".0" — bit-exact for the same
//     inputs on every machine and every run. This canonical form is what the
//     content pipeline (EpisodeCompiler) and the runtime contracts consume,
//     so every host speaks ONE shape.
//   - The schema itself is data-driven: register_kind_json() loads a kind's
//     schema (validated all-or-nothing), schema_to_json() emits the whole
//     registry canonically (bit-exact round-trip) — the machine-readable
//     surface the editor/scripting/CLI/MCP hosts bind to.
//   - All refusals are all-or-nothing with a deterministic diagnostic; a
//     refused validation never yields a partial canonical document.
//
// PURE and DETERMINISTIC — the semantic core never touches a concrete world.
// Self-contained (std).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace semantic {

// The declared type of a schema field. The semantic core type-checks the
// document against it before canonicalizing.
enum class SemanticFieldType : std::uint8_t {
    String = 0,      // JSON string; `allowed` constrains the value set
    Int = 1,         // JSON number with an integral value (emitted without ".0")
    Double = 2,      // JSON number (finite; min/max apply)
    Bool = 3,        // JSON true/false
    StringArray = 4  // JSON array of strings (order preserved)
};

// One declared field of a kind's schema.
struct SemanticField {
    std::string name;   // document key (non-empty, unique within the kind)
    SemanticFieldType type{ SemanticFieldType::String };
    bool required{ false };

    // Default for missing optional fields (only when hasDefault). The
    // default is type-matched at registration (all-or-nothing).
    bool hasDefault{ false };
    double defaultNumber{ 0.0 };
    std::string defaultString;
    bool defaultBool{ false };
    std::vector<std::string> defaultStrings;

    // Constraints (checked at registration: min <= max when both set).
    double min{ 0.0 };
    bool hasMin{ false };
    double max{ 0.0 };
    bool hasMax{ false };
    // Allowed values for String fields (non-empty = constrained).
    std::vector<std::string> allowed;
};

// A content kind: name (the kind the hosts address), version, and the schema
// fields in DECLARATION order (the canonical key order).
struct SemanticKind {
    std::string name;  // non-empty, unique within the registry
    int version{ 1 };
    std::vector<SemanticField> fields;
};

class ISemanticApi {
public:
    virtual ~ISemanticApi() = default;

    // Registers a kind's schema programmatically. Refuses (all-or-nothing)
    // an empty/duplicate kind name, an invalid version, a non-empty kind
    // with no fields, a duplicate/empty field name, an unknown field type,
    // a min > max, an `allowed` constraint on a non-String field, or a
    // type-mismatched default.
    virtual bool register_kind(const SemanticKind& kind,
                               std::string& errorOut) = 0;

    // Loads a kind's schema from its canonical JSON document (the same shape
    // schema_to_json() emits). All-or-nothing.
    virtual bool register_kind_json(const std::string& schemaJson,
                                    std::string& errorOut) = 0;

    // Kind names in registration order.
    virtual std::vector<std::string> kinds() const = 0;

    // The schema of one kind (nullptr when the kind is not registered).
    virtual const SemanticKind* kind(const std::string& name) const = 0;

    // Canonical JSON of one kind's schema (or of the whole registry).
    virtual std::string kind_schema_to_json(const std::string& name) const = 0;
    virtual std::string schema_to_json() const = 0;

    // Validates `document` against the kind's schema and, on success,
    // produces the CANONICAL document in `canonicalOut` (keys in schema
    // order, defaults applied, %.9g numbers, integral ints without ".0").
    // Refuses (all-or-nothing, canonicalOut untouched) on an unknown kind, a
    // malformed document, an unknown document key, a missing required field,
    // a type mismatch, an out-of-range number, or a disallowed string.
    virtual bool validate(const std::string& kind, const std::string& document,
                          std::string& canonicalOut,
                          std::string& errorOut) = 0;
};

// The only implementation (src/engine/sdk/SemanticApi.cpp).
std::unique_ptr<ISemanticApi> create_semantic_api();

}  // namespace semantic
}  // namespace engine
