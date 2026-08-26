// VehicleDamageTests — gate do contrato IVehicleDamage (§6 item 57, dano de
// veículo CORE): prova dano por peça (clamp, destruição em 0, destacamento
// por limiar), peças destruídas/destacadas fora de combate, repair_all,
// recusas all-or-nothing e round-trip JSON.

#include "engine/vehicles/IVehicleDamage.hpp"

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

bool near(float a, float b, float eps = 1.0e-4f) { return (a > b - eps) && (a < b + eps); }

bool strings_equal(const std::vector<std::string>& actual,
                   const std::vector<std::string>& expected, const char* what) {
    bool ok = actual.size() == expected.size();
    if (ok) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] != expected[i]) ok = false;
        }
    }
    check(ok, what);
    return ok;
}

std::vector<engine::vehicles::VehiclePartSpec> make_parts() {
    std::vector<engine::vehicles::VehiclePartSpec> parts;
    engine::vehicles::VehiclePartSpec wheel;
    wheel.name = "wheel_fl";
    wheel.maxHealth = 100.0f;
    wheel.detachable = true;
    wheel.detachThreshold = 0.3f;
    parts.push_back(wheel);
    engine::vehicles::VehiclePartSpec door;
    door.name = "door_l";
    door.maxHealth = 80.0f;
    door.detachable = true;
    door.detachThreshold = 0.25f;
    parts.push_back(door);
    engine::vehicles::VehiclePartSpec enginePart;
    enginePart.name = "engine";
    enginePart.maxHealth = 200.0f;
    parts.push_back(enginePart);
    return parts;
}

void test_damage() {
    auto damage = engine::vehicles::create_vehicle_damage();
    std::string error;
    check(damage->configure(make_parts(), error), "configure 3 peças");
    check(near(damage->health("wheel_fl"), 100.0f), "saúde inicial");

    auto result = damage->apply_damage("wheel_fl", 40.0f);
    check(near(damage->health("wheel_fl"), 60.0f), "dano reduz a saúde");
    check(near(result.totalDamage, 40.0f), "totalDamage = 40");
    check(result.newlyDestroyed.empty() && result.newlyDetached.empty(),
          "sem destruição/destaque ainda");

    // Dança acima da saúde → clamp em 0 + destruição.
    result = damage->apply_damage("wheel_fl", 100.0f);
    check(damage->health("wheel_fl") == 0.0f, "clamp em 0");
    strings_equal(result.newlyDestroyed,
                  std::vector<std::string>{ "wheel_fl" }, "wheel_fl destruída");
    check(damage->is_destroyed("wheel_fl"), "is_destroyed");

    // Destruída não recebe mais dano.
    result = damage->apply_damage("wheel_fl", 50.0f);
    check(result.totalDamage == 0.0f && damage->health("wheel_fl") == 0.0f,
          "destruída fora de combate");

    // Destacamento por limiar (door: 80 max, limiar 0.25 → 20).
    result = damage->apply_damage("door_l", 61.0f);  // 80-61 = 19 <= 20
    check(damage->is_detached("door_l") && !damage->is_destroyed("door_l"),
          "door destacada (não destruída)");
    strings_equal(result.newlyDetached,
                  std::vector<std::string>{ "door_l" }, "newlyDetached");

    // Peça desconhecida.
    result = damage->apply_damage("ghost", 10.0f);
    check(result.totalDamage == 0.0f && result.newlyDestroyed.empty(),
          "peça desconhecida → 0 dano");

    // Engine: nunca destaca (não-destacável), só destrói.
    damage->apply_damage("engine", 200.0f);
    check(damage->is_destroyed("engine") && !damage->is_detached("engine"),
          "engine destrói sem destacar");

    strings_equal(damage->destroyed_parts(),
                  std::vector<std::string>{ "engine", "wheel_fl" },
                  "destroyed em ordem crescente");
    strings_equal(damage->detached_parts(), std::vector<std::string>{ "door_l" },
                  "detached em ordem");
}

void test_repair_and_refusals() {
    auto damage = engine::vehicles::create_vehicle_damage();
    std::string error;
    check(damage->configure(make_parts(), error), "configure");
    damage->apply_damage("wheel_fl", 100.0f);
    damage->apply_damage("door_l", 61.0f);
    damage->repair_all();
    check(near(damage->health("wheel_fl"), 100.0f) && !damage->is_destroyed("wheel_fl"),
          "repair restaura saúde e estados");
    check(!damage->is_detached("door_l"), "repair re-anexa");

    const std::string intact = damage->to_json();
    std::vector<engine::vehicles::VehiclePartSpec> bad;
    bad.push_back({ "a", 100.0f, false, 0.0f });
    bad.push_back({ "a", 100.0f, false, 0.0f });
    check(!damage->configure(bad, error), "nome duplicado recusa");
    bad.clear();
    bad.push_back({ "", 100.0f, false, 0.0f });
    check(!damage->configure(bad, error), "nome vazio recusa");
    bad.clear();
    bad.push_back({ "b", 0.0f, false, 0.0f });
    check(!damage->configure(bad, error), "maxHealth 0 recusa");
    bad.clear();
    bad.push_back({ "c", 100.0f, true, 1.5f });
    check(!damage->configure(bad, error), "detachThreshold > 1 recusa");
    check(damage->to_json() == intact, "estado intacto após recusas");
}

void test_json() {
    auto a = engine::vehicles::create_vehicle_damage();
    auto b = engine::vehicles::create_vehicle_damage();
    std::string error;
    check(a->configure(make_parts(), error), "configure A");
    a->apply_damage("wheel_fl", 100.0f);
    a->apply_damage("door_l", 61.0f);

    check(b->load_from_json(a->to_json(), error), "load B");
    check(b->to_json() == a->to_json(), "round-trip bit-exact");
    check(b->is_destroyed("wheel_fl") && b->is_detached("door_l"),
          "estados preservados no round-trip");
    check(!b->load_from_json(R"({"version":2,"parts":[]})", error), "versão 2 recusa");
}

}  // namespace

int main() {
    test_damage();
    test_repair_and_refusals();
    test_json();

    if (failures == 0) {
        std::printf("vehicle_damage_tests: all checks passed\n");
        return 0;
    }
    std::printf("vehicle_damage_tests: %d failure(s)\n", failures);
    return 1;
}
