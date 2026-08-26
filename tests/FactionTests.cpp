// FactionTests — gate do contrato público de facções/equipes (agente 4 §1 item
// 16 "equipe"). Prova que as relações simétricas, o registro, as queries e o
// JSON são all-or-nothing/bit-exact/determinísticos como documentado.

#include "engine/gameplay/IFaction.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

using engine::gameplay::FactionRelation;
using engine::gameplay::FactionSpec;
using engine::gameplay::create_faction;

FactionSpec make_spec() {
    FactionSpec spec;
    spec.teams = {"player", "guard", "bandit"};
    spec.relations = {{"player", "guard", FactionRelation::Friendly},
                      {"guard", "bandit", FactionRelation::Hostile}};
    return spec;
}

void test_validate_all_or_nothing() {
    FactionSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    FactionSpec bad = s;
    bad.teams.push_back("");  // vazio
    check(!bad.validate(err) && !err.empty(), "id vazio recusa");

    bad = s;
    bad.teams.push_back("player");  // duplicado
    check(!bad.validate(err) && !err.empty(), "id duplicado recusa");

    bad = s;
    bad.relations.push_back({"nope", "guard", FactionRelation::Hostile});
    check(!bad.validate(err) && !err.empty(), "team desconhecido recusa");
}

void test_spec_json_roundtrip() {
    const FactionSpec spec = make_spec();
    const std::string json = spec.to_json();
    FactionSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    FactionSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
}

void test_relations_symmetric() {
    auto f = create_faction();
    std::string err;
    check(f->configure(make_spec(), err), "configure");

    // spec define player↔guard friendly
    check(f->is_friendly("player", "guard") && f->is_friendly("guard", "player"),
          "player↔guard friendly (simétrico)");
    // spec define guard↔bandit hostile
    check(f->is_hostile("guard", "bandit") && f->is_hostile("bandit", "guard"),
          "guard↔bandit hostile (simétrico)");
    // não definido = neutral
    check(f->relation("player", "bandit") == FactionRelation::Neutral,
          "player↔bandit neutral (default)");
    // uma equipe consigo mesma = friendly
    check(f->is_friendly("player", "player"), "equipe consigo = friendly");
}

void test_dynamic_team_registration() {
    auto f = create_faction();
    std::string err;
    check(f->configure(make_spec(), err), "configure");

    check(f->register_team("dragon", err) && err.empty(), "register dragon");
    check(!f->register_team("dragon", err) && !err.empty(), "dup recusa");
    check(!f->is_hostile("dragon", "player") &&
              f->relation("dragon", "player") == FactionRelation::Neutral,
          "nova equipe inicia neutra com existentes");

    check(f->set_relation("dragon", "player", FactionRelation::Hostile, err) &&
              err.empty(),
          "set_relation dragon↔player hostile");
    check(f->is_hostile("dragon", "player") && f->is_hostile("player", "dragon"),
          "set_relation é simétrica");

    check(!f->set_relation("dragon", "unicorn", FactionRelation::Friendly, err) &&
              !err.empty(),
          "set_relation com equipe desconhecida recusa");
}

void test_teams_ordered() {
    auto f = create_faction();
    std::string err;
    check(f->configure(make_spec(), err), "configure");

    const auto ts = f->teams();
    check(ts.size() == 3, "3 equipes");
    check(ts[0] < ts[1] && ts[1] < ts[2], "teams ordenadas (determinístico)");
}

void test_state_roundtrip() {
    auto f = create_faction();
    std::string err;
    check(f->configure(make_spec(), err), "configure");
    f->register_team("rats", err);
    f->set_relation("rats", "player", FactionRelation::Hostile, err);

    const std::string state = f->serialize_state();
    auto g = create_faction();
    check(g->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(g->serialize_state() == state, "state round-trip bit-exact");
    check(g->teams() == f->teams(), "teams restauradas");
    check(g->is_hostile("rats", "player"), "relações restauradas");

    check(!g->deserialize_state("{bad", err) && !err.empty(), "estado inválido recusa");
}

void test_determinism() {
    auto a = create_faction();
    auto b = create_faction();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");
    a->register_team("aliens", err);
    b->register_team("aliens", err);
    a->set_relation("aliens", "bandit", FactionRelation::Hostile, err);
    b->set_relation("aliens", "bandit", FactionRelation::Hostile, err);

    check(a->serialize_state() == b->serialize_state(),
          "determinismo: estados bit-exatos cross-instance");
    check(a->teams() == b->teams(), "determinismo: teams idênticas");
    check(a->is_hostile("aliens", "bandit") == b->is_hostile("aliens", "bandit"),
          "determinismo: queries idênticas");
}

}  // namespace

int main() {
    test_validate_all_or_nothing();
    test_spec_json_roundtrip();
    test_relations_symmetric();
    test_dynamic_team_registration();
    test_teams_ordered();
    test_state_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "faction_tests: all checks passed\n";
    } else {
        std::cout << "faction_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}