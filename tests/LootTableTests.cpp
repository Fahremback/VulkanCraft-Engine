// LootTableTests — gate do contrato público de loot tables (agente 4 §1 item
// 17). Prova que a rolagem por seed é determinística cross-instance,
// all-or-nothing no load, bit-exact no round-trip JSON, e que seleção
// ponderada/chance/count/merge/validação se comportam como documentado.

#include "engine/registry/ILootTable.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
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

using engine::registry::ILootTable;
using engine::registry::LootEntry;
using engine::registry::LootRoll;
using engine::registry::LootTableSpec;
using engine::registry::create_loot_table;

LootTableSpec make_spec() {
    LootTableSpec spec;
    spec.id = "vulkancraft:zombie";
    spec.rolls_min = 2;
    spec.rolls_max = 2;
    spec.entries = {
        {"vulkancraft:iron_ingot", 1.0, 1, 3, 1.0},
        {"vulkancraft:rotten_flesh", 3.0, 1, 1, 0.8},
    };
    return spec;
}

void test_spec_validate() {
    LootTableSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    LootTableSpec bad = s;
    bad.id = "";
    check(!bad.validate(err) && !err.empty(), "id vazio recusa");

    bad = s;
    bad.entries[0].item = "";
    check(!bad.validate(err) && !err.empty(), "item vazio recusa");

    bad = s;
    bad.entries[0].weight = 0.0;
    check(!bad.validate(err) && !err.empty(), "weight 0 recusa");

    bad = s;
    bad.entries[0].count_max = 0;  // < count_min 1
    check(!bad.validate(err) && !err.empty(), "count_max < count_min recusa");

    bad = s;
    bad.entries[0].chance = 1.5;
    check(!bad.validate(err) && !err.empty(), "chance > 1 recusa");

    bad = s;
    bad.rolls_max = 1;  // < rolls_min 2
    check(!bad.validate(err) && !err.empty(), "rolls_max < rolls_min recusa");
}

void test_spec_json_roundtrip() {
    const LootTableSpec spec = make_spec();
    const std::string json = spec.to_json();
    LootTableSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    LootTableSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json("{\"version\":2,\"id\":\"x\",\"entries\":[]}", err) &&
              !err.empty(),
          "versão desconhecida recusa");
}

void test_determinism_cross_instance() {
    auto a = create_loot_table();
    auto b = create_loot_table();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");

    const auto ra = a->roll(UINT64_C(123456789));
    const auto rb = b->roll(UINT64_C(123456789));
    check(ra.size() == rb.size(), "determinismo: mesmo número de itens");
    for (std::size_t i = 0; i < ra.size() && i < rb.size(); ++i) {
        check(ra[i].item == rb[i].item && ra[i].count == rb[i].count,
              "determinismo: itens/counts idênticos em ordem");
    }

    // rolls_min=rolls_max → número de rolls fixo; com chance 1 e count fixo o
    // total é exato.
    LootTableSpec fixed;
    fixed.id = "t";
    fixed.rolls_min = 3;
    fixed.rolls_max = 3;
    fixed.entries = {{"a", 1.0, 2, 2, 1.0}};
    auto c = create_loot_table();
    check(c->configure(fixed, err), "configure fixed");
    const auto rc = c->roll(UINT64_C(7));
    check(rc.size() == 1 && rc[0].item == "a" && rc[0].count == 6,
          "3 rolls × count 2 = 6 (chance 1, count fixo)");
}

void test_chance_zero_and_bounds() {
    LootTableSpec none;
    none.id = "never";
    none.rolls_min = 5;
    none.rolls_max = 5;
    none.entries = {{"x", 1.0, 1, 1, 0.0}};
    auto a = create_loot_table();
    std::string err;
    check(a->configure(none, err), "configure chance 0");
    check(a->roll(UINT64_C(1)).empty(), "chance 0 → nada dropa");

    LootTableSpec range;
    range.id = "range";
    range.rolls_min = 4;
    range.rolls_max = 4;
    range.entries = {{"y", 1.0, 1, 5, 1.0}};
    auto b = create_loot_table();
    check(b->configure(range, err), "configure range");
    const auto rb = b->roll(UINT64_C(42));
    check(rb.size() == 1 && rb[0].item == "y" && rb[0].count >= 4 &&
              rb[0].count <= 20,
          "4 rolls × count 1..5 → total em [4,20]");
}

void test_weighted_dominance() {
    LootTableSpec spec;
    spec.id = "dom";
    spec.rolls_min = 100;
    spec.rolls_max = 100;
    spec.entries = {
        {"common", 1000000000.0, 1, 1, 1.0},
        {"rare", 1.0, 1, 1, 1.0},
    };
    auto a = create_loot_table();
    std::string err;
    check(a->configure(spec, err), "configure dom");
    const auto rolls = a->roll(UINT64_C(99));
    int common = 0, rare = 0;
    for (const auto& r : rolls) {
        if (r.item == "common") common = r.count;
        if (r.item == "rare") rare = r.count;
    }
    check(common > 0, "item pesado aparece");
    check(common >= rare, "item pesado domina o raro");
}

void test_merge_same_item() {
    LootTableSpec spec;
    spec.id = "merge";
    spec.rolls_min = 2;
    spec.rolls_max = 2;
    spec.entries = {
        {"iron", 1.0, 1, 1, 1.0},
        {"iron", 1.0, 1, 1, 1.0},
    };
    auto a = create_loot_table();
    std::string err;
    check(a->configure(spec, err), "configure merge");
    const auto rolls = a->roll(UINT64_C(5));
    check(rolls.size() == 1 && rolls[0].item == "iron" && rolls[0].count == 2,
          "mesmo item mergeado em um único LootRoll");
}

void test_items_and_validate() {
    auto a = create_loot_table();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    const auto items = a->items();
    check(items.size() == 2 && items[0] == "vulkancraft:iron_ingot" &&
              items[1] == "vulkancraft:rotten_flesh",
          "items() distinct sorted");

    check(a->validate_items({"vulkancraft:iron_ingot", "vulkancraft:rotten_flesh"})
              .empty(),
          "catálogo completo → consistente");
    const auto missing = a->validate_items({"vulkancraft:rotten_flesh"});
    check(missing.size() == 1 && missing[0] == "vulkancraft:iron_ingot",
          "item ausente do catálogo → listado sorted");
}

void test_state_roundtrip() {
    auto a = create_loot_table();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    const std::string state = a->serialize_state();

    auto b = create_loot_table();
    check(b->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(b->serialize_state() == state, "state round-trip bit-exact");

    const auto ra = a->roll(UINT64_C(77));
    const auto rb = b->roll(UINT64_C(77));
    check(ra.size() == rb.size(), "rolls idênticos pós-restauração");
    for (std::size_t i = 0; i < ra.size() && i < rb.size(); ++i) {
        check(ra[i].item == rb[i].item && ra[i].count == rb[i].count,
              "rolls idênticos pós-restauração (conteúdo)");
    }

    auto c = create_loot_table();
    const std::string before = c->serialize_state();
    check(!c->deserialize_state(
              "{\"version\":1,\"id\":\"x\",\"rolls_min\":1,\"rolls_max\":1,"
              "\"entries\":[{\"item\":\"a\",\"weight\":0}]}",
              err) &&
              !err.empty(),
          "spec inválida recusa no deserialize (all-or-nothing)");
    check(c->serialize_state() == before, "recusa não muta (all-or-nothing)");
}

void test_seed_dependence() {
    auto a = create_loot_table();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    const auto r1 = a->roll(UINT64_C(1));
    const auto r2 = a->roll(UINT64_C(2));
    // Rolls diferentes NÃO precisam divergir (colisão possível), mas o
    // contrato garante: a mesma seed sempre produz o mesmo resultado.
    const auto r1b = a->roll(UINT64_C(1));
    check(r1.size() == r1b.size(), "mesma seed → mesmo tamanho");
    for (std::size_t i = 0; i < r1.size() && i < r1b.size(); ++i) {
        check(r1[i].item == r1b[i].item && r1[i].count == r1b[i].count,
              "mesma seed → mesmo conteúdo");
    }
    (void)r2;
}

}  // namespace

int main() {
    test_spec_validate();
    test_spec_json_roundtrip();
    test_determinism_cross_instance();
    test_chance_zero_and_bounds();
    test_weighted_dominance();
    test_merge_same_item();
    test_items_and_validate();
    test_state_roundtrip();
    test_seed_dependence();

    if (g_failures == 0) {
        std::cout << "loot_table_tests: all checks passed\n";
    } else {
        std::cout << "loot_table_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
