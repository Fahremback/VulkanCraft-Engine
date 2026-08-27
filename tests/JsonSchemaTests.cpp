// JsonSchemaTests.cpp
//
// Gate for IJsonSchema (agente 2 — item `glaze`): JSON document validation
// against declared schemas, using the ENGINE'S SHARED JSON PARSER (JsonMini
// — the consolidation authority). Proves the REAL adapter
// (src/engine/sdk/JsonSchema.cpp):
//   - required fields must be present;
//   - type mismatches refused (bool/number/string/array);
//   - number ranges enforced;
//   - strict mode rejects unknown fields;
//   - non-strict allows extra fields;
//   - malformed JSON refused;
//   - determinism: same doc + schema -> identical verdict.
//
// Deterministic and headless. Self-contained (std + engine/ui + JsonMini).

#include <engine/ui/IJsonSchema.hpp>

#include <cstdio>
#include <memory>
#include <string>

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

engine::ui::JsonSchemaSpec make_spec() {
    engine::ui::JsonSchemaSpec spec;
    spec.name = "item";
    spec.strict = true;
    spec.fields = {
        { "name", engine::ui::SchemaFieldType::String, true, 0, 0, false },
        { "count", engine::ui::SchemaFieldType::Number, true, 0, 100, true },
        { "enabled", engine::ui::SchemaFieldType::Bool, false, 0, 0, false },
        { "tags", engine::ui::SchemaFieldType::Array, false, 0, 0, false },
    };
    return spec;
}

void test_required_and_types() {
    std::printf("[jschema] required/types…\n");
    auto schema = engine::ui::create_json_schema();
    const auto spec = make_spec();
    std::string err;

    // Valid document.
    check(schema->validate(spec,
              "{\"name\":\"rock\",\"count\":42,\"enabled\":true,\"tags\":[]}",
              err),
          "valid document accepted");
    // Missing required field.
    check(!schema->validate(spec, "{\"name\":\"rock\"}", err),
          "missing required 'count' refused");
    check(!err.empty(), "refusal reports a message");
    // Type mismatch: count is a string.
    err.clear();
    check(!schema->validate(spec, "{\"name\":\"rock\",\"count\":\"many\"}", err),
          "count as string refused (must be number)");
    // name as number.
    err.clear();
    check(!schema->validate(spec, "{\"name\":3,\"count\":1}", err),
          "name as number refused (must be string)");
    // enabled as string.
    err.clear();
    check(!schema->validate(spec,
              "{\"name\":\"x\",\"count\":1,\"enabled\":\"yes\"}", err),
          "enabled as string refused (must be bool)");
    // tags as object.
    err.clear();
    check(!schema->validate(spec,
              "{\"name\":\"x\",\"count\":1,\"tags\":{}}", err),
          "tags as object refused (must be array)");
    std::printf("[jschema] required/types OK\n");
}

void test_ranges() {
    std::printf("[jschema] ranges…\n");
    auto schema = engine::ui::create_json_schema();
    const auto spec = make_spec();
    std::string err;

    check(schema->validate(spec, "{\"name\":\"x\",\"count\":0}", err),
          "count=0 (min) accepted");
    check(schema->validate(spec, "{\"name\":\"x\",\"count\":100}", err),
          "count=100 (max) accepted");
    err.clear();
    check(!schema->validate(spec, "{\"name\":\"x\",\"count\":-1}", err),
          "count=-1 (below min) refused");
    err.clear();
    check(!schema->validate(spec, "{\"name\":\"x\",\"count\":101}", err),
          "count=101 (above max) refused");
    std::printf("[jschema] ranges OK\n");
}

void test_strict() {
    std::printf("[jschema] strict…\n");
    auto schema = engine::ui::create_json_schema();
    auto spec = make_spec();
    std::string err;

    // Unknown field refused in strict mode.
    check(!schema->validate(spec, "{\"name\":\"x\",\"count\":1,\"extra\":2}", err),
          "unknown field refused (strict)");
    // Non-strict allows extra fields.
    spec.strict = false;
    err.clear();
    check(schema->validate(spec, "{\"name\":\"x\",\"count\":1,\"extra\":2}", err),
          "unknown field allowed (non-strict)");
    std::printf("[jschema] strict OK\n");
}

void test_malformed() {
    std::printf("[jschema] malformed…\n");
    auto schema = engine::ui::create_json_schema();
    const auto spec = make_spec();
    std::string err;

    check(!schema->validate(spec, "{nope", err),
          "malformed JSON refused");
    check(!err.empty(), "refusal reports a message");
    err.clear();
    check(!schema->validate(spec, "[]", err),
          "root array refused (must be object)");
    std::printf("[jschema] malformed OK\n");
}

void test_determinism() {
    std::printf("[jschema] determinism…\n");
    auto a = engine::ui::create_json_schema();
    auto b = engine::ui::create_json_schema();
    const auto spec = make_spec();
    std::string ea, eb;
    const std::string doc = "{\"name\":\"rock\",\"count\":42,\"tags\":[\"a\"]}";
    const bool va = a->validate(spec, doc, ea);
    const bool vb = b->validate(spec, doc, eb);
    check(va == vb, "same verdict across instances");
    check(va, "valid doc accepted by both");
    std::printf("[jschema] determinism OK\n");
}

}  // namespace

int main() {
    test_required_and_types();
    test_ranges();
    test_strict();
    test_malformed();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[jschema] ALL PASSED\n");
        return 0;
    }
    std::printf("[jschema] %d FAILURE(S)\n", g_failures);
    return 1;
}
