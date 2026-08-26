// EquipmentTests — gate do contrato público de equipamento/slots (agente 4 §1
// item 17, parte "equipamentos"). Prova que a validação por tags, equip/
// unequip e a persistência são determinísticas, all-or-nothing e bit-exact.

#include "engine/registry/IEquipment.hpp"

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

using engine::registry::EquipmentCategory;
using engine::registry::EquipmentSpec;
using engine::registry::IEquipment;
using engine::registry::create_equipment;

EquipmentSpec make_spec() {
    EquipmentSpec spec;
    spec.categories = {
        {"head", {"vulkancraft:armor"}},
        {"chest", {"vulkancraft:armor"}},
        {"hand", {"vulkancraft:tool"}},
        {"offhand", {}},  // sem restrição
    };
    return spec;
}

void test_spec_validate() {
    EquipmentSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    EquipmentSpec bad = s;
    bad.categories[0].id = "";
    check(!bad.validate(err) && !err.empty(), "category id vazio recusa");

    bad = s;
    bad.categories[1].id = "head";  // duplicado
    check(!bad.validate(err) && !err.empty(), "category duplicada recusa");

    bad = s;
    bad.categories[2].tags = {"", "x"};
    check(!bad.validate(err) && !err.empty(), "tag vazia recusa");

    bad = s;
    bad.categories[2].tags = {"tool", "tool"};
    check(!bad.validate(err) && !err.empty(), "tag duplicada na categoria recusa");
}

void test_spec_json_roundtrip() {
    const EquipmentSpec spec = make_spec();
    const std::string json = spec.to_json();
    EquipmentSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    EquipmentSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json("{\"version\":2,\"categories\":[]}", err) &&
              !err.empty(),
          "versão desconhecida recusa");
}

void test_equip_validation() {
    auto e = create_equipment();
    std::string err;
    check(e->configure(make_spec(), err), "configure");
    check(e->equip("head", "vulkancraft:iron_helmet",
                   {"vulkancraft:armor", "vulkancraft:iron"}, err),
          "capacete com tag armor cabe em head");
    check(e->equipped("head", err) == "vulkancraft:iron_helmet",
          "equipped head = capacete");
    check(!e->equip("head", "vulkancraft:pickaxe",
                    {"vulkancraft:tool"}, err) &&
              !err.empty(),
          "tool não cabe em head (tag errada)");
    check(e->equipped("head", err) == "vulkancraft:iron_helmet",
          "recusa não substitui o equipado");

    check(e->equip("hand", "vulkancraft:pickaxe",
                   {"vulkancraft:tool"}, err),
          "tool cabe em hand");
    check(e->equip("offhand", "vulkancraft:torch", {"vulkancraft:block"}, err),
          "offhand sem restrição aceita qualquer item");

    check(!e->equip("nope", "x", {}, err) && !err.empty(),
          "categoria desconhecida recusa");
    check(!e->equip("hand", "", {}, err) && !err.empty(), "item vazio recusa");
}

void test_unequip_and_items() {
    auto e = create_equipment();
    std::string err;
    check(e->configure(make_spec(), err), "configure");
    e->equip("head", "vulkancraft:helmet", {"vulkancraft:armor"}, err);
    e->equip("chest", "vulkancraft:plate", {"vulkancraft:armor"}, err);
    e->equip("hand", "vulkancraft:sword", {"vulkancraft:tool"}, err);

    const auto items = e->items();
    check(items.size() == 3 && items[0].first == "chest" &&
              items[1].first == "hand" && items[2].first == "head",
          "items() sorted por categoria");
    check(items[0].second == "vulkancraft:plate", "items() conteúdo");

    check(e->unequip("hand", err), "unequip hand");
    check(e->equipped("hand", err).empty(), "hand vazio");
    check(e->items().size() == 2, "items() sem o slot esvaziado");
    check(e->unequip("hand", err), "unequip de slot vazio = no-op aceito");
    check(!e->unequip("nope", err) && !err.empty(), "unequip categoria desconhecida recusa");

    const auto cats = e->categories();
    check(cats.size() == 4 && cats[0] == "head" && cats[3] == "offhand",
          "categories() na ordem de declaração");
}

void test_replace() {
    auto e = create_equipment();
    std::string err;
    check(e->configure(make_spec(), err), "configure");
    e->equip("hand", "vulkancraft:sword", {"vulkancraft:tool"}, err);
    check(e->equip("hand", "vulkancraft:axe", {"vulkancraft:tool"}, err),
          "equip substitui o item anterior (slot único)");
    check(e->equipped("hand", err) == "vulkancraft:axe", "hand = machado");
    check(e->items().size() == 1, "slot único → um item por categoria");
}

void test_state_roundtrip() {
    auto a = create_equipment();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    a->equip("head", "vulkancraft:helmet", {"vulkancraft:armor"}, err);
    a->equip("hand", "vulkancraft:pickaxe", {"vulkancraft:tool"}, err);

    const std::string state = a->serialize_state();
    auto b = create_equipment();
    check(b->configure(make_spec(), err), "configure b");
    check(b->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(b->serialize_state() == state, "state round-trip bit-exact");
    check(b->equipped("head", err) == "vulkancraft:helmet" &&
              b->equipped("hand", err) == "vulkancraft:pickaxe",
          "slots restaurados");

    auto c = create_equipment();
    check(c->configure(make_spec(), err), "configure c");
    const std::string before = c->serialize_state();
    check(!c->deserialize_state("{\"slots\":{\"nope\":\"x\"}}", err) &&
              !err.empty(),
          "categoria desconhecida recusa (all-or-nothing)");
    check(c->serialize_state() == before, "recusa não muta (all-or-nothing)");
}

void test_determinism() {
    auto a = create_equipment();
    auto b = create_equipment();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");
    a->equip("head", "vulkancraft:h1", {"vulkancraft:armor"}, err);
    b->equip("head", "vulkancraft:h1", {"vulkancraft:armor"}, err);
    a->equip("chest", "vulkancraft:c1", {"vulkancraft:armor"}, err);
    b->equip("chest", "vulkancraft:c1", {"vulkancraft:armor"}, err);
    a->unequip("chest", err);
    b->unequip("chest", err);
    check(a->serialize_state() == b->serialize_state(),
          "determinismo: estado bit-exato");
    const auto ia = a->items();
    const auto ib = b->items();
    check(ia.size() == ib.size(), "determinismo: items iguais");
    for (std::size_t i = 0; i < ia.size() && i < ib.size(); ++i) {
        check(ia[i].first == ib[i].first && ia[i].second == ib[i].second,
              "determinismo: conteúdo idêntico");
    }
}

}  // namespace

int main() {
    test_spec_validate();
    test_spec_json_roundtrip();
    test_equip_validation();
    test_unequip_and_items();
    test_replace();
    test_state_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "equipment_tests: all checks passed\n";
    } else {
        std::cout << "equipment_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
