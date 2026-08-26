// ReflectionTests — gate do contrato IReflection (§1 item 50, reflection
// CORE): prova registro de tipos/campos, consultas (type_names ordenado,
// field_names, has_field), JSON versionado all-or-nothing e round-trip.

#include "engine/entity/IReflection.hpp"

#include <cstdio>
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

}  // namespace

int main() {
    test_register_query();
    test_refusals();
    test_json_roundtrip();

    if (failures == 0) {
        std::printf("reflection_tests: all checks passed\n");
        return 0;
    }
    std::printf("reflection_tests: %d failure(s)\n", failures);
    return 1;
}
