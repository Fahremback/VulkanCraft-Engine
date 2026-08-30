// ReflectionTests — gate do contrato IReflection (§1 item 50, reflection
// CORE): prova registro de tipos/campos, consultas (type_names ordenado,
// field_names, has_field), JSON versionado all-or-nothing, round-trip — e o
// WIRING por-tool da fachada semântica (§8 item 1, linha 114): o artifact
// schema/reflection/tools.json (gerado por tools/sdk/tool-wiring.mjs da
// MESMA fonte única semanticToolDefinitions()) é carregado pela runtime REAL
// e TODOS os tipos registram.

#include "engine/entity/IReflection.hpp"
#include "engine/sdk/RegistryJson.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

engine::entity::TypeInfo make_type(const std::string& name) {
    engine::entity::TypeInfo type;
    type.name = name;
    return type;
}

void test_register_query() {
    auto reflection = engine::entity::create_reflection();
    std::string error;

    engine::entity::TypeInfo health = make_type("health");
    health.fields.push_back({ "max", engine::entity::FieldKind::Float });
    health.fields.push_back({ "regen", engine::entity::FieldKind::Float });
    check(reflection->register_type(health, error), "register health");
    check(reflection->register_type(make_type("physics"), error), "register physics");
    check(reflection->count() == 2, "2 tipos");

    const engine::entity::TypeInfo* found = reflection->type("health");
    check(found != nullptr && found->fields.size() == 2, "type health com 2 campos");
    check(reflection->type("ghost") == nullptr, "type desconhecido → nullptr");

    const std::vector<std::string> names = reflection->type_names();
    check(names.size() == 2 && names[0] == "health" && names[1] == "physics",
          "type_names em ordem crescente");

    const std::vector<std::string> fields = reflection->field_names("health");
    check(fields.size() == 2 && fields[0] == "max" && fields[1] == "regen",
          "field_names na ordem de declaração");
    check(reflection->has_field("health", "regen") && !reflection->has_field("health", "x"),
          "has_field");
    check(reflection->field_names("ghost").empty(), "field_names de desconhecido → vazio");
}

void test_refusals() {
    auto reflection = engine::entity::create_reflection();
    std::string error;
    check(reflection->register_type(make_type("a"), error), "register 'a'");
    const std::string intact = reflection->to_json();

    check(!reflection->register_type(make_type(""), error), "nome vazio recusa");
    check(!reflection->register_type(make_type("a"), error), "tipo duplicado recusa");

    engine::entity::TypeInfo badField = make_type("b");
    badField.fields.push_back({ "", engine::entity::FieldKind::Float });
    check(!reflection->register_type(badField, error), "campo vazio recusa");
    badField.fields.clear();
    badField.fields.push_back({ "x", engine::entity::FieldKind::Int });
    badField.fields.push_back({ "x", engine::entity::FieldKind::Bool });
    check(!reflection->register_type(badField, error), "campo duplicado recusa");
    check(reflection->to_json() == intact && reflection->count() == 1,
          "estado intacto após recusas");
}

void test_json_roundtrip() {
    auto a = engine::entity::create_reflection();
    auto b = engine::entity::create_reflection();
    std::string error;

    engine::entity::TypeInfo mob = make_type("mob");
    mob.fields.push_back({ "speed", engine::entity::FieldKind::Float });
    mob.fields.push_back({ "kind", engine::entity::FieldKind::Enum });
    mob.fields.push_back({ "position", engine::entity::FieldKind::Vec3 });
    a->register_type(mob, error);
    a->register_type(make_type("item"), error);

    check(b->load_from_json(a->to_json(), error), "load do JSON de A");
    check(b->to_json() == a->to_json(), "round-trip bit-exact");
    check(b->field_names("mob").size() == 3 &&
              b->has_field("mob", "position"),
          "campos preservados");

    check(!b->load_from_json(R"({"version":2,"types":[]})", error), "versão 2 recusa");
    check(!b->load_from_json(R"({"version":1,"types":[{"name":"x","fields":[{"name":"f","kind":"ghost"}]}]})", error),
          "kind desconhecido recusa");
    check(b->to_json() == a->to_json(), "estado intacto após recusas JSON");
}

void test_wired_semantic_types() {
    // §8 item 1 — o wiring por-tool: cada tool da fachada semântica vira um
    // TypeInfo no artifact committed (schema/reflection/tools.json, gerado
    // por tools/sdk/tool-wiring.mjs da fonte única). Esta prova carrega o
    // artifact na runtime REAL (create_reflection + load_from_json) e exige
    // que TODOS os tipos registrem — o wiring não é documental.
#ifdef VULKANCRAFT_SOURCE_DIR
    const std::string artifact =
        std::string(VULKANCRAFT_SOURCE_DIR) + "/schema/reflection/tools.json";
    std::ifstream in(artifact);
    if (!in) {
        check(false, "artifact schema/reflection/tools.json existe");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), {});

    engine::sdk::JsonValue root;
    std::string parseErr;
    check(engine::sdk::json_parse(text, root, parseErr), "artifact JSON válido");
    const auto* typesArr = root.field("types");
    check(typesArr && typesArr->is_array() && !typesArr->array.empty(),
          "artifact tem types");
    if (!typesArr || !typesArr->is_array() || typesArr->array.empty()) return;
    const std::size_t expectedCount = typesArr->array.size();

    auto reflection = engine::entity::create_reflection();
    std::string error;
    check(reflection->load_from_json(text, error),
          "artifact carrega na runtime IReflection");
    if (error.empty() && reflection->count() == expectedCount) {
        std::printf("  wired: %zu tipos semânticos registrados na runtime\n",
                    reflection->count());
    }
    check(reflection->count() == expectedCount,
          "count == número de tipos no artifact");

    // Spot-checks: tools conhecidas da fachada + campos derivados do schema.
    check(reflection->type("game_capabilities") != nullptr, "game_capabilities wired");
    check(reflection->type("create_game_project") != nullptr, "create_game_project wired");
    const auto* project = reflection->type("create_game_project");
    check(project && project->stable_id == "semantic.create_game_project",
          "stable_id canônico semantic.<tool>");
    check(project && reflection->has_field("create_game_project", "name") &&
              reflection->has_field("create_game_project", "profile"),
          "campos derivados do inputSchema");
    check(reflection->type("create_shader_asset") != nullptr, "create_shader_asset wired");
    check(reflection->type("author_render_graph") != nullptr, "author_render_graph wired");
    check(reflection->type("create_light_asset") != nullptr, "create_light_asset wired");

    // Round-trip estável: re-emitir e recarregar preserva a contagem.
    const std::string re = reflection->to_json();
    auto b = engine::entity::create_reflection();
    std::string err2;
    check(b->load_from_json(re, err2) && b->count() == expectedCount,
          "round-trip to_json→load preserva os tipos");
#else
    check(false, "VULKANCRAFT_SOURCE_DIR definido (wiring test ativo)");
#endif
}

}  // namespace

int main() {
    test_register_query();
    test_refusals();
    test_json_roundtrip();
    test_wired_semantic_types();

    if (failures == 0) {
        std::printf("reflection_tests: all checks passed\n");
        return 0;
    }
    std::printf("reflection_tests: %d failure(s)\n", failures);
    return 1;
}
