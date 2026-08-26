// EntityArchetypeTests — gate do contrato IEntityArchetype (§1 item 13,
// archetypes CORE): prova registro por kind (player/mob/vehicle/projectile/
// interactive), find, nomes ordenados, componentes com JSON opaco, JSON
// versionado all-or-nothing (kind desconhecido, nome vazio/duplicado,
// componente malformado) e round-trip bit-exact.

#include "engine/entity/IEntityArchetype.hpp"

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

engine::entity::EntityArchetype make_archetype(const std::string& name,
                                               engine::entity::EntityKind kind) {
    engine::entity::EntityArchetype archetype;
    archetype.name = name;
    archetype.kind = kind;
    return archetype;
}

void test_register_and_find() {
    auto registry = engine::entity::create_entity_archetype_registry();
    std::string error;

    engine::entity::EntityArchetype player = make_archetype("hero", engine::entity::EntityKind::Player);
    player.components.push_back({ "health", R"({"max":100})" });
    player.components.push_back({ "physics", R"({"radius":0.4})" });
    check(registry->register_archetype(player, error), "register hero (player)");

    check(registry->register_archetype(make_archetype("zombie", engine::entity::EntityKind::Mob), error),
          "register zombie (mob)");
    check(registry->register_archetype(make_archetype("arrow", engine::entity::EntityKind::Projectile), error),
          "register arrow (projectile)");
    check(registry->register_archetype(make_archetype("chest", engine::entity::EntityKind::Interactive), error),
          "register chest (interactive)");
    check(registry->register_archetype(make_archetype("sedan", engine::entity::EntityKind::Vehicle), error),
          "register sedan (vehicle)");
    check(registry->count() == 5, "5 archetypes");

    const engine::entity::EntityArchetype* found = registry->find("hero");
    check(found != nullptr && found->kind == engine::entity::EntityKind::Player,
          "find hero → kind player");
    check(found->components.size() == 2 && found->components[0].type == "health",
          "componentes preservados");
    check(registry->find("ghost") == nullptr, "find de desconhecido → nullptr");

    const std::vector<std::string> names = registry->names();
    check(names.size() == 5 && names[0] == "arrow" && names[4] == "zombie",
          "names em ordem crescente (arrow..zombie)");
}

void test_refusals() {
    auto registry = engine::entity::create_entity_archetype_registry();
    std::string error;
    check(registry->register_archetype(make_archetype("a", engine::entity::EntityKind::Mob), error),
          "register 'a'");
    const std::string intact = registry->to_json();

    engine::entity::EntityArchetype empty;
    check(!registry->register_archetype(empty, error), "nome vazio recusa");
    check(!registry->register_archetype(make_archetype("a", engine::entity::EntityKind::Mob), error),
          "duplicata recusa");
    engine::entity::EntityArchetype badComp = make_archetype("b", engine::entity::EntityKind::Mob);
    badComp.components.push_back({ "", R"({})" });
    check(!registry->register_archetype(badComp, error), "tipo de componente vazio recusa");
    engine::entity::EntityArchetype badJson = make_archetype("c", engine::entity::EntityKind::Mob);
    badJson.components.push_back({ "x", "{" });
    check(!registry->register_archetype(badJson, error), "JSON de componente malformado recusa");
    check(registry->to_json() == intact && registry->count() == 1,
          "estado intacto após recusas");
}

void test_json_roundtrip() {
    auto a = engine::entity::create_entity_archetype_registry();
    auto b = engine::entity::create_entity_archetype_registry();
    std::string error;

    engine::entity::EntityArchetype hero = make_archetype("hero", engine::entity::EntityKind::Player);
    hero.components.push_back({ "health", R"({"max":100})" });
    a->register_archetype(hero, error);
    a->register_archetype(make_archetype("zombie", engine::entity::EntityKind::Mob), error);

    check(b->load_from_json(a->to_json(), error), "load do JSON de A");
    check(b->to_json() == a->to_json(), "round-trip bit-exact");
    check(b->find("zombie") != nullptr && b->find("zombie")->kind == engine::entity::EntityKind::Mob,
          "kind preservado no round-trip");

    check(!b->load_from_json(R"({"version":2,"archetypes":[]})", error), "versão 2 recusa");
    check(!b->load_from_json(R"({"version":1,"archetypes":[{"name":"x","kind":"ghost"}]})", error),
          "kind desconhecido recusa");
    check(b->to_json() == a->to_json(), "estado intacto após recusas JSON");
}

}  // namespace

int main() {
    test_register_and_find();
    test_refusals();
    test_json_roundtrip();

    if (failures == 0) {
        std::printf("entity_archetype_tests: all checks passed\n");
        return 0;
    }
    std::printf("entity_archetype_tests: %d failure(s)\n", failures);
    return 1;
}
