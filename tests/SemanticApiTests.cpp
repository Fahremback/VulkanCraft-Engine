// SemanticApiTests.cpp
//
// FALTANTES differential — SemanticEngineAPI: ONE surface for C++, editor,
// scripting, CLI and MCP (META §32). The gate drives the PUBLIC contract
// headless and proves:
//   - the kind-agnostic semantic core: a content kind declares its SCHEMA
//     once (fields: name/type/required/default/min/max/allowed) and every
//     host validates through the SAME deterministic surface;
//   - canonicalization is bit-exact: keys in schema declaration order,
//     defaults applied for missing optional fields, %.9g numbers, integral
//     ints without ".0", string arrays preserved;
//   - the schema itself is data-driven and round-trips bit-exactly
//     (schema_to_json -> register_kind_json -> identical emission);
//   - ONE surface from BOTH binding paths: the programmatic C++ path and the
//     JSON host path (MCP/CLI/editor bind to the JSON schema) produce
//     bit-identical schemas and bit-identical canonical documents;
//   - cross-instance determinism (two instances -> identical canonical form
//     and identical schema emission);
//   - all-or-nothing refusals: unknown kind, malformed/non-object document,
//     unknown document key (never guessed), missing required field, type
//     mismatch, non-integral int, min/max violation, disallowed string,
//     non-string array element — a refused validation yields NO canonical
//     output;
//   - schema registration is all-or-nothing (duplicate kind, empty name,
//     bad version, empty fields, duplicate field, unknown type, min > max,
//     allowed on a non-string, non-integral int default, malformed schema);
//   - COMPOSITION with EpisodeCompiler: the semantic canonical form feeds
//     the content pipeline — canonical documents compile and verify as
//     episode entries, with the compiler's validator bound to the SAME
//     semantic surface.

#include "engine/compiler/IEpisodeCompiler.hpp"
#include "engine/semantic/ISemanticApi.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

using engine::semantic::SemanticField;
using engine::semantic::SemanticFieldType;
using engine::semantic::SemanticKind;
using engine::semantic::create_semantic_api;

// The two schema documents the gate registers (canonical JSON — the shape
// schema_to_json() emits, which the editor/scripting/CLI/MCP hosts bind to).
const char* kPhysicsMaterialSchema =
    "{\"name\":\"physics_material\",\"version\":1,\"fields\":["
    "{\"name\":\"name\",\"type\":\"string\",\"required\":true},"
    "{\"name\":\"friction\",\"type\":\"double\",\"default\":0.6,\"min\":0,\"max\":1},"
    "{\"name\":\"restitution\",\"type\":\"double\",\"default\":0.1,\"min\":0,\"max\":1},"
    "{\"name\":\"flags\",\"type\":\"stringArray\",\"default\":[\"solid\"]},"
    "{\"name\":\"enabled\",\"type\":\"bool\",\"default\":true}]}";

const char* kItemSchema =
    "{\"name\":\"item\",\"version\":1,\"fields\":["
    "{\"name\":\"id\",\"type\":\"string\",\"required\":true},"
    "{\"name\":\"stack\",\"type\":\"int\",\"default\":64,\"min\":1,\"max\":9999},"
    "{\"name\":\"category\",\"type\":\"string\",\"allowed\":[\"tool\",\"material\",\"food\"]},"
    "{\"name\":\"tags\",\"type\":\"stringArray\"}]}";

// ---- 1. schema JSON round-trip + kinds ----

void test_schema_roundtrip() {
    auto api = create_semantic_api();
    std::string error;
    check(api->register_kind_json(kPhysicsMaterialSchema, error),
          "physics_material schema registered");
    check(error.empty(), "schema diagnostic empty");
    check(api->register_kind_json(kItemSchema, error), "item schema registered");

    const std::vector<std::string> kinds = api->kinds();
    check(kinds.size() == 2 && kinds[0] == "physics_material" &&
              kinds[1] == "item",
          "kinds in registration order");

    // Round-trip: re-registering the emitted registry reproduces it
    // bit-exactly.
    const std::string emitted = api->schema_to_json();
    check(!emitted.empty(), "schema_to_json non-empty");
    auto api2 = create_semantic_api();
    // Split the registry document into its two kind documents is NOT needed:
    // register_kind_json takes one kind at a time, so emit per kind.
    const std::string pm = api->kind_schema_to_json("physics_material");
    const std::string item = api->kind_schema_to_json("item");
    check(api2->register_kind_json(pm, error), "physics_material re-registered");
    check(api2->register_kind_json(item, error), "item re-registered");
    check(api2->schema_to_json() == emitted,
          "schema registry round-trips bit-exactly");
    check(api2->kind_schema_to_json("physics_material") == pm &&
              api2->kind_schema_to_json("item") == item,
          "per-kind schema emission round-trips bit-exactly");

    // The schema is self-describing: every field carries its full contract.
    check(pm.find("\"name\":\"friction\",\"type\":\"double\",\"required\":false,"
                  "\"default\":0.6,\"min\":0,\"max\":1") != std::string::npos,
          "schema carries type/required/default/min/max for every field");
    check(item.find("\"allowed\":[\"tool\",\"material\",\"food\"]") !=
              std::string::npos,
          "schema carries the allowed-value set");

    std::printf("[semantic] schema JSON round-trip + self-describing OK\n");
}

// ---- 2. canonicalization (defaults, order, %.9g, integral ints) ----

void test_canonicalization() {
    auto api = create_semantic_api();
    std::string error;
    check(api->register_kind_json(kPhysicsMaterialSchema, error),
          "physics_material registered");
    check(api->register_kind_json(kItemSchema, error), "item registered");

    std::string canonical;
    check(api->validate("physics_material",
                        "{\"name\":\"Ice\",\"friction\":0.4,\"enabled\":false}",
                        canonical, error),
          "partial physics_material doc validates");
    check(canonical ==
              "{\"name\":\"Ice\",\"friction\":0.4,\"restitution\":0.1,"
              "\"flags\":[\"solid\"],\"enabled\":false}",
          "canonical: schema-order keys + defaults applied + %.9g");

    check(api->validate("physics_material",
                        "{\"name\":\"Grass\",\"flags\":[\"solid\",\"walkable\"]}",
                        canonical, error),
          "flags-override doc validates");
    check(canonical ==
              "{\"name\":\"Grass\",\"friction\":0.6,\"restitution\":0.1,"
              "\"flags\":[\"solid\",\"walkable\"],\"enabled\":true}",
          "canonical: given array preserved, remaining defaults applied");

    check(api->validate("item", "{\"id\":\"vc:emerald\",\"category\":\"material\","
                                "\"tags\":[\"gem\"]}",
                        canonical, error),
          "item doc validates");
    check(canonical ==
              "{\"id\":\"vc:emerald\",\"stack\":64,\"category\":\"material\","
              "\"tags\":[\"gem\"]}",
          "canonical: item defaults applied");

    check(api->validate("item", "{\"id\":\"x\",\"stack\":5}", canonical, error),
          "int doc validates");
    check(canonical == "{\"id\":\"x\",\"stack\":5}",
          "integral int emitted without '.0'");

    std::printf("[semantic] canonicalization (defaults/order/%.9g) OK\n", 0.5);
}

// ---- 3. one surface, two binding paths + cross-instance determinism ----

void test_two_binding_paths_and_determinism() {
    std::string error;

    // C++ programmatic path.
    auto programmatic = create_semantic_api();
    // The programmatic kind mirrors the JSON schema FIELD FOR FIELD, so the
    // two binding paths emit the identical schema and canonicalize the same
    // document identically.
    SemanticKind kind;
    kind.name = "physics_material";
    kind.version = 1;
    SemanticField name;
    name.name = "name";
    name.type = SemanticFieldType::String;
    name.required = true;
    SemanticField friction;
    friction.name = "friction";
    friction.type = SemanticFieldType::Double;
    friction.hasDefault = true;
    friction.defaultNumber = 0.6;
    friction.hasMin = true;
    friction.min = 0.0;
    friction.hasMax = true;
    friction.max = 1.0;
    SemanticField restitution;
    restitution.name = "restitution";
    restitution.type = SemanticFieldType::Double;
    restitution.hasDefault = true;
    restitution.defaultNumber = 0.1;
    restitution.hasMin = true;
    restitution.min = 0.0;
    restitution.hasMax = true;
    restitution.max = 1.0;
    SemanticField flags;
    flags.name = "flags";
    flags.type = SemanticFieldType::StringArray;
    flags.hasDefault = true;
    flags.defaultStrings = { "solid" };
    SemanticField enabled;
    enabled.name = "enabled";
    enabled.type = SemanticFieldType::Bool;
    enabled.hasDefault = true;
    enabled.defaultBool = true;
    kind.fields = { name, friction, restitution, flags, enabled };
    check(programmatic->register_kind(kind, error), "programmatic register");

    // JSON host path (what MCP/CLI/editor bind to).
    auto jsonHost = create_semantic_api();
    check(jsonHost->register_kind_json(kPhysicsMaterialSchema, error),
          "JSON-host register");

    // Both paths agree bit-exactly on the emitted schema.
    check(programmatic->kind_schema_to_json("physics_material") ==
              jsonHost->kind_schema_to_json("physics_material"),
          "both binding paths emit the identical schema");

    // Both paths validate the same document to the identical canonical form.
    std::string canonP, canonJ;
    check(programmatic->validate("physics_material",
                                 "{\"name\":\"Ice\",\"friction\":0.4}",
                                 canonP, error),
          "programmatic host validates");
    check(jsonHost->validate("physics_material",
                             "{\"name\":\"Ice\",\"friction\":0.4}", canonJ,
                             error),
          "JSON host validates");
    check(canonP == canonJ, "one surface: identical canonical from both hosts");

    // Cross-instance determinism on the JSON path.
    auto api2 = create_semantic_api();
    check(api2->register_kind_json(kPhysicsMaterialSchema, error),
          "second instance registered");
    std::string canon2;
    check(api2->validate("physics_material", "{\"name\":\"Ice\",\"friction\":0.4}",
                         canon2, error),
          "second instance validates");
    check(canon2 == canonP && api2->schema_to_json() ==
                  jsonHost->schema_to_json(),
          "cross-instance bit-identical canonical + schema");

    std::printf("[semantic] one surface, two binding paths, determinism OK\n");
}

// ---- 4. document refusals (all-or-nothing) ----

void test_document_refusals() {
    auto api = create_semantic_api();
    std::string error;
    check(api->register_kind_json(kPhysicsMaterialSchema, error),
          "physics_material registered");
    check(api->register_kind_json(kItemSchema, error), "item registered");

    std::string canonical;
    const auto& refused = [&](const char* kind, const char* doc,
                              const char* needle, const char* message) {
        check(!api->validate(kind, doc, canonical, error) &&
                  !error.empty() &&
                  (needle == nullptr ||
                   error.find(needle) != std::string::npos) &&
                  canonical.empty(),
              message);
    };

    refused("unknown_kind", "{}", "unknown semantic kind", "unknown kind refused");
    refused("item", "{not json", "malformed document", "malformed doc refused");
    refused("item", "[1,2]", "must be a JSON object",
            "non-object document refused");
    refused("item", "{\"id\":\"x\",\"color\":\"red\"}", "unknown field 'color'",
            "unknown document key refused (never guessed)");
    refused("physics_material", "{\"friction\":0.2}",
            "missing required field 'name'", "missing required refused");
    refused("item", "{\"id\":42}", "field 'id' must be a string",
            "type mismatch refused");
    refused("item", "{\"id\":\"x\",\"stack\":5.5}",
            "must be an integral number", "non-integral int refused");
    refused("item", "{\"id\":\"x\",\"stack\":0}", "below minimum 1",
            "min violation refused");
    refused("item", "{\"id\":\"x\",\"stack\":10000}", "above maximum 9999",
            "max violation refused");
    refused("item", "{\"id\":\"x\",\"category\":\"weapon\"}",
            "value 'weapon' is not allowed", "allowed-set violation refused");
    refused("item", "{\"id\":\"x\",\"tags\":[\"a\",5]}",
            "must be an array of strings", "non-string array element refused");

    std::printf("[semantic] document refusals all-or-nothing OK\n");
}

// ---- 5. schema registration refusals ----

void test_schema_refusals() {
    auto api = create_semantic_api();
    std::string error;

    check(!api->register_kind_json(
              "{\"name\":\"\",\"version\":1,\"fields\":["
              "{\"name\":\"a\",\"type\":\"string\"}]}",
              error) &&
              !error.empty(),
          "empty kind name refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":0,\"fields\":["
              "{\"name\":\"a\",\"type\":\"string\"}]}",
              error) &&
              !error.empty(),
          "version 0 refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":1,\"fields\":[]}", error) &&
              !error.empty(),
          "empty fields refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":1,\"fields\":["
              "{\"name\":\"a\",\"type\":\"string\"},"
              "{\"name\":\"a\",\"type\":\"string\"}]}",
              error) &&
              error.find("duplicate field") != std::string::npos,
          "duplicate field refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":1,\"fields\":["
              "{\"name\":\"a\",\"type\":\"float\"}]}",
              error) &&
              error.find("unknown type") != std::string::npos,
          "unknown field type refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":1,\"fields\":["
              "{\"name\":\"a\",\"type\":\"double\",\"min\":5,\"max\":1}]}",
              error) &&
              error.find("min > max") != std::string::npos,
          "min > max refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":1,\"fields\":["
              "{\"name\":\"a\",\"type\":\"double\",\"allowed\":[\"x\"]}]}",
              error) &&
              !error.empty(),
          "allowed on non-string refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":1,\"fields\":["
              "{\"name\":\"a\",\"type\":\"int\",\"default\":1.5}]}",
              error) &&
              !error.empty(),
          "non-integral int default refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":1,\"fields\":["
              "{\"name\":\"a\",\"type\":\"string\",\"default\":5}]}",
              error) &&
              !error.empty(),
          "type-mismatched default refused");
    check(!api->register_kind_json("{not json", error) && !error.empty(),
          "malformed schema refused");
    check(!api->register_kind_json(
              "{\"name\":\"k\",\"version\":1,\"fields\":{}}", error) &&
              !error.empty(),
          "fields not an array refused");

    // Duplicate KIND refused, and the registry is untouched by refusals.
    check(api->register_kind_json(kItemSchema, error), "item registered");
    check(!api->register_kind_json(kItemSchema, error) &&
              error.find("already registered") != std::string::npos,
          "duplicate kind refused");
    check(api->kinds().size() == 1, "registry untouched by refusals");

    std::printf("[semantic] schema registration refusals OK\n");
}

// ---- 6. composition: semantic surface feeds EpisodeCompiler ----

void test_composition_with_episode_compiler() {
    std::string error;

    // The semantic core canonicalizes the content.
    auto api = create_semantic_api();
    check(api->register_kind_json(kPhysicsMaterialSchema, error),
          "physics_material registered");
    std::string ice, ground;
    check(api->validate("physics_material", "{\"name\":\"Ice\",\"friction\":0.4}",
                        ice, error),
          "ice canonicalized");
    check(api->validate("physics_material",
                        "{\"name\":\"Ground\",\"friction\":0.8,\"enabled\":false}",
                        ground, error),
          "ground canonicalized");

    // The compiler's validator is bound to the SAME semantic surface — the
    // canonical documents are the episode entries.
    auto compiler = engine::compiler::create_episode_compiler();
    check(compiler->register_validator(
              "physics_material",
              [&api](const std::string& json, std::string& e) {
                  std::string canonical;
                  return api->validate("physics_material", json, canonical, e);
              },
              error),
          "semantic validator bound to the compiler");
    check(compiler->register_simulator(
              "physics_material",
              [](const std::string& json, int steps, std::string& trace,
                 std::string&) {
                  trace = "sim:" + std::to_string(steps) + ":" +
                          std::to_string(json.size());
                  return true;
              },
              error),
          "simulator registered");
    check(compiler->register_tester(
              "physics_material",
              [](const std::string&, std::string&) { return true; }, error),
          "tester registered");

    engine::compiler::EpisodeManifest manifest;
    manifest.title = "SemanticEpisode";
    engine::compiler::EpisodeEntry entryIce;
    entryIce.kind = "physics_material";
    entryIce.name = "ice";
    entryIce.json = ice;
    engine::compiler::EpisodeEntry entryGround;
    entryGround.kind = "physics_material";
    entryGround.name = "ground";
    entryGround.json = ground;
    manifest.entries = { entryIce, entryGround };

    engine::compiler::EpisodePackage package;
    check(compiler->compile(manifest, package, error),
          "semantic canonical forms compile as an episode");
    check(compiler->verify(package, error), "episode verifies");
    check(package.signature.size() == 32, "published signature intact");
    check(package.manifestJson.find("\"name\":\"ice\"") != std::string::npos &&
              package.manifestJson.find("\"name\":\"ground\"") !=
                  std::string::npos,
          "manifest carries the semantic-derived entries");

    // A non-canonical (raw) document is refused by the same validator — the
    // pipeline only accepts the semantic surface's canonical form.
    engine::compiler::EpisodeEntry raw;
    raw.kind = "physics_material";
    raw.name = "raw";
    raw.json = "{\"friction\":0.4}";  // missing required 'name'
    manifest.entries.push_back(raw);
    engine::compiler::EpisodePackage refusedPackage;
    check(!compiler->compile(manifest, refusedPackage, error) &&
              !error.empty(),
          "non-canonical raw entry refused by the semantic validator");

    std::printf("[semantic] composition with EpisodeCompiler OK\n");
}

}  // namespace

int main() {
    test_schema_roundtrip();
    test_canonicalization();
    test_two_binding_paths_and_determinism();
    test_document_refusals();
    test_schema_refusals();
    test_composition_with_episode_compiler();
    if (g_failures == 0) {
        std::printf("[semantic-api] ALL PASSED\n");
        return 0;
    }
    std::printf("[semantic-api] %d FAILURE(S)\n", g_failures);
    return 1;
}
