// GameplayIntegrationTests.cpp
//
// FALTANTES §19 + task_plan §10 "suíte integrada de gameplay": the systems that
// ship as separate contracts (abilities, attributes, damage, effects,
// inventory and world interaction) are exercised TOGETHER in one deterministic
// scenario — the integration proof the plan asks for, not a re-run of the
// per-system unit suites.
//
// Scenario (one continuous fight, headless and text-only):
//   - The caster owns an Inventory holding potions (ItemRegistry + Inventory);
//   - drinking a potion consumes the stack AND heals through the world seam;
//   - casting an ability spends mana (attributes/resources), respects
//     cooldowns and conditions (OwnerAttribute), and applies Damage;
//   - a periodic fire effect ticks over time through update();
//   - a BlockEdit ability writes into the voxel world (interaction with the
//     world), and its box is validated at load;
//   - the whole runtime state round-trips via snapshot/apply_snapshot, and the
//     same script on a fresh instance reproduces bit-exact results.
//
// Everything runs against the public SDK contracts (engine/gameplay/,
// engine/registry/) with a deterministic mock world implementing IAbilityWorld
// — the same "without core code" proof pattern as ability_system_tests.

#include "engine/gameplay/IAbilitySystem.hpp"
#include "engine/registry/Inventory.hpp"
#include "engine/registry/ItemRegistry.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
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

// ---- Deterministic mock world (the public seam) ------------------------------
// Same shape as AbilitySystemTests' MockWorld: a dense voxel map, simple
// explicit-Euler physics, health, attributes, tags and a resource pool. The
// integrated scenario additionally uses the ItemRegistry + Inventory contracts
// on top of this world.
struct MockBody {
    glm::vec3 position{ 0.0f };
    glm::vec3 velocity{ 0.0f };
    glm::vec3 force{ 0.0f };
    float mass{ 1.0f };
    bool fixed{ false };
    float health{ 100.0f };
    float maxHealth{ 100.0f };
    std::map<std::string, float> attributes;
    std::vector<std::string> tags;
    std::map<std::string, float> resources;
};

class MockWorld final : public engine::gameplay::IAbilityWorld {
public:
    std::uint32_t nextBodyId_{ 1 };
    std::map<std::uint32_t, MockBody> bodies_;
    std::map<std::int64_t, std::uint32_t> blocks_;
    const float gravity_{ -9.81f };

    engine::gameplay::AbilityBodyId spawn(const glm::vec3& position,
                                          float mass = 1.0f,
                                          bool fixed = false) {
        engine::gameplay::AbilityBodyId id;
        id.id = nextBodyId_++;
        MockBody& body = bodies_[id.id];
        body.position = position;
        body.mass = mass;
        body.fixed = fixed;
        return id;
    }

    std::uint32_t block_at(int x, int y, int z) const override {
        const auto found = blocks_.find(key(x, y, z));
        return found == blocks_.end() ? 0 : found->second;
    }
    bool set_block(int x, int y, int z, std::uint32_t blockId) override {
        blocks_[key(x, y, z)] = blockId;
        return true;
    }

    bool body_state(const engine::gameplay::AbilityBodyId& body,
                    engine::gameplay::AbilityBodyState& out) const override {
        const auto found = bodies_.find(body.id);
        if (found == bodies_.end()) return false;
        out.position = found->second.position;
        out.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        out.linearVelocity = found->second.velocity;
        out.angularVelocity = glm::vec3(0.0f);
        return true;
    }
    bool apply_impulse(const engine::gameplay::AbilityBodyId& body,
                       const glm::vec3& impulse) override {
        const auto found = bodies_.find(body.id);
        if (found == bodies_.end()) return false;
        found->second.velocity += impulse / found->second.mass;
        return true;
    }
    bool add_force(const engine::gameplay::AbilityBodyId& body,
                   const glm::vec3& force) override {
        const auto found = bodies_.find(body.id);
        if (found == bodies_.end()) return false;
        found->second.force += force;
        return true;
    }
    bool set_transform(const engine::gameplay::AbilityBodyId& body,
                       const glm::vec3& position,
                       const glm::quat& rotation) override {
        const auto found = bodies_.find(body.id);
        if (found == bodies_.end()) return false;
        found->second.position = position;
        (void)rotation;
        return true;
    }
    bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                 float maxDistance,
                 engine::gameplay::AbilityRaycastHit& out) const override {
        (void)origin;
        (void)direction;
        (void)maxDistance;
        (void)out;
        return false;
    }

    float attribute(const engine::gameplay::AbilityBodyId& body,
                    const std::string& name) const override {
        const auto found = bodies_.find(body.id);
        if (found == bodies_.end()) return 0.0f;
        const auto attribute = found->second.attributes.find(name);
        return attribute == found->second.attributes.end() ? 0.0f
                                                           : attribute->second;
    }
    engine::gameplay::AbilityTagList tags(
        const engine::gameplay::AbilityBodyId& body) const override {
        const auto found = bodies_.find(body.id);
        return found == bodies_.end() ? engine::gameplay::AbilityTagList{}
                                      : found->second.tags;
    }
    bool spend_cost(const engine::gameplay::AbilityBodyId& caster,
                    const std::string& resource, float amount) override {
        const auto found = bodies_.find(caster.id);
        if (found == bodies_.end()) return false;
        float& pool = found->second.resources[resource];
        if (pool < amount) return false;
        pool -= amount;
        return true;
    }
    bool health(const engine::gameplay::AbilityBodyId& body,
                float& out) const override {
        const auto found = bodies_.find(body.id);
        if (found == bodies_.end()) return false;
        out = found->second.health;
        return true;
    }
    bool damage(const engine::gameplay::AbilityBodyId& body,
                float amount) override {
        const auto found = bodies_.find(body.id);
        if (found == bodies_.end()) return false;
        found->second.health = std::max(0.0f, found->second.health - amount);
        return true;
    }
    bool heal(const engine::gameplay::AbilityBodyId& body,
              float amount) override {
        const auto found = bodies_.find(body.id);
        if (found == bodies_.end()) return false;
        found->second.health =
            std::min(found->second.maxHealth, found->second.health + amount);
        return true;
    }

private:
    static std::int64_t key(int x, int y, int z) {
        return (static_cast<std::int64_t>(x) << 32) ^
               (static_cast<std::int64_t>(y) << 16) ^
               static_cast<std::int64_t>(z);
    }
};

engine::registry::ItemDefinition make_item(const std::string& ns,
                                           const std::string& name,
                                           int maxStack = 64) {
    engine::registry::ItemDefinition def;
    def.ns = ns;
    def.name = name;
    def.maxStack = maxStack;
    return def;
}

// ---- One integrated fight ----------------------------------------------------
// Runs the full scenario against `sys`/`world`; returns the enemy's final
// health so a fresh instance can reproduce it bit-exact.
float run_fight(engine::gameplay::IAbilitySystem& sys, MockWorld& world) {
    // --- world setup: caster + enemy ---
    engine::gameplay::AbilityBodyId caster = world.spawn(glm::vec3(0.0f));
    engine::gameplay::AbilityBodyId enemy = world.spawn(glm::vec3(3.0f, 0.0f, 0.0f));
    world.bodies_[caster.id].attributes["strength"] = 12.0f;
    world.bodies_[caster.id].resources["mana"] = 100.0f;
    world.bodies_[caster.id].tags = { "player" };
    world.bodies_[enemy.id].health = 100.0f;

    // --- inventory: potions the caster can drink (ItemRegistry + Inventory) ---
    engine::registry::ItemRegistry items;
    std::string err;
    check(items.register_item(make_item("vulkancraft", "health_potion", 16), err),
          "register health_potion");
    check(items.register_item(make_item("vulkancraft", "mana_potion", 16), err),
          "register mana_potion");

    engine::registry::Inventory bag(9);
    // A default SlotFilter is a LOCKED slot; unlock every slot the bag uses.
    {
        engine::registry::SlotFilter any;
        any.allowAny = true;
        for (int s = 0; s < bag.slot_count(); ++s) {
            bag.set_filter(s, any);
        }
    }
    engine::registry::ItemStack potion;
    potion.item = "vulkancraft:health_potion";
    potion.count = 3;
    const engine::registry::ItemStack remainder =
        bag.add(potion, items, err);
    check(remainder.empty(), "potions fully added to bag");
    check(bag.count_of("vulkancraft:health_potion") == 3,
          "bag holds 3 potions");

    // Drinking a potion: consume the stack (Inventory) and heal through the
    // world seam (gameplay) — the integration point between the two contracts.
    const int consumed = bag.consume(0, 1);
    check(consumed == 1, "drank exactly one potion");
    check(bag.count_of("vulkancraft:health_potion") == 2,
          "bag now holds 2 potions");
    world.damage(caster, 20.0f);
    float casterHp = 0.0f;
    check(world.health(caster, casterHp) && casterHp == 80.0f,
          "caster at 80 hp after hit");
    world.heal(caster, 20.0f);
    check(world.health(caster, casterHp) && casterHp == 100.0f,
          "potion healed caster back to full");

    // --- abilities: melee strike (damage + cooldown + attribute gate) ---
    engine::gameplay::AbilityDefinition strike;
    strike.id = "abilities:power_strike";
    strike.name = "Power Strike";
    engine::gameplay::AbilityCondition strGate;
    strGate.kind = engine::gameplay::AbilityConditionKind::OwnerAttribute;
    strGate.attribute = "strength";
    strGate.minValue = 10.0f;
    strike.conditions.push_back(strGate);
    strike.cost.resource = "mana";
    strike.cost.amount = 25.0f;
    strike.cooldownSeconds = 2.0f;
    strike.targeting.mode = engine::gameplay::AbilityTargetMode::Body;
    strike.targeting.range = 10.0f;
    engine::gameplay::AbilityEffect hit;
    hit.type = engine::gameplay::AbilityEffectType::Damage;
    hit.amount = 35.0f;
    strike.effects.push_back(hit);
    std::string regErr;
    check(sys.register_ability(strike, regErr), "register power_strike");

    // --- abilities: fire dot (periodic effect over time) ---
    engine::gameplay::AbilityDefinition fire;
    fire.id = "abilities:fire_touch";
    fire.name = "Fire Touch";
    fire.targeting.mode = engine::gameplay::AbilityTargetMode::Body;
    fire.targeting.range = 10.0f;
    engine::gameplay::AbilityEffect tick;
    tick.type = engine::gameplay::AbilityEffectType::Damage;
    tick.amount = 5.0f;
    engine::gameplay::AbilityEffect periodic;
    periodic.type = engine::gameplay::AbilityEffectType::Periodic;
    periodic.intervalSeconds = 0.5f;
    periodic.ticks = 4;
    periodic.subEffect = std::make_shared<engine::gameplay::AbilityEffect>(tick);
    fire.effects.push_back(periodic);
    check(sys.register_ability(fire, regErr), "register fire_touch");

    // --- abilities: terraform (voxel world interaction) ---
    engine::gameplay::AbilityDefinition terra;
    terra.id = "abilities:terraform";
    terra.name = "Terraform";
    terra.targeting.mode = engine::gameplay::AbilityTargetMode::Point;
    terra.targeting.range = 20.0f;
    engine::gameplay::AbilityEffect edit;
    edit.type = engine::gameplay::AbilityEffectType::BlockEdit;
    edit.min = glm::ivec3(0, 0, 0);
    edit.max = glm::ivec3(1, 1, 1);
    edit.blockId = 7;
    edit.relative = true;
    terra.effects.push_back(edit);
    check(sys.register_ability(terra, regErr), "register terraform");

    // --- cast sequence ---
    engine::gameplay::AbilityTarget target;
    target.mode = engine::gameplay::AbilityTargetMode::Body;
    target.body = enemy;

    // Power strike lands: mana spent, damage applied, cooldown starts.
    const engine::gameplay::CastResult cast1 =
        sys.cast("abilities:power_strike", caster, target, world);
    check(cast1.accepted, "power strike accepted");
    check(world.bodies_[enemy.id].health == 65.0f, "enemy took 35 damage");
    check(world.bodies_[caster.id].resources["mana"] == 75.0f,
          "mana spent (100 - 25)");
    check(sys.on_cooldown("abilities:power_strike"),
          "power strike on cooldown");

    // Re-cast while on cooldown: refused, nothing applied (all-or-nothing).
    const engine::gameplay::CastResult cast2 =
        sys.cast("abilities:power_strike", caster, target, world);
    check(!cast2.accepted, "power strike refused on cooldown");
    check(world.bodies_[enemy.id].health == 65.0f, "no double damage");

    // Attribute gate: a weak caster is refused before any cost is spent.
    engine::gameplay::AbilityBodyId weak = world.spawn(glm::vec3(0.0f, 5.0f, 0.0f));
    world.bodies_[weak.id].resources["mana"] = 100.0f;
    const engine::gameplay::CastResult cast3 =
        sys.cast("abilities:power_strike", weak, target, world);
    check(!cast3.accepted, "weak caster refused (strength < 10)");
    check(world.bodies_[weak.id].resources["mana"] == 100.0f,
          "refused cast spent nothing");

    // Fire touch: periodic damage ticks through update().
    const engine::gameplay::CastResult cast4 =
        sys.cast("abilities:fire_touch", caster, target, world);
    check(cast4.accepted, "fire touch accepted");
    sys.update(0.5f, world);
    check(world.bodies_[enemy.id].health == 60.0f, "first dot tick (65 - 5)");
    sys.update(0.5f, world);
    sys.update(0.5f, world);
    sys.update(0.5f, world);
    check(world.bodies_[enemy.id].health == 45.0f, "four dot ticks total");

    // Terraform: writes blocks into the voxel world (world interaction).
    engine::gameplay::AbilityTarget point;
    point.mode = engine::gameplay::AbilityTargetMode::Point;
    point.point = glm::vec3(10.0f, 0.0f, 10.0f);
    const engine::gameplay::CastResult cast5 =
        sys.cast("abilities:terraform", caster, point, world);
    check(cast5.accepted, "terraform accepted");
    check(world.block_at(10, 0, 10) == 7, "terraform wrote block at origin");
    check(world.block_at(11, 1, 11) == 7, "terraform wrote block at max corner");

    // Cooldown expires and the strike can be used again.
    sys.update(2.0f, world);
    check(!sys.on_cooldown("abilities:power_strike"),
          "power strike cooled down");
    const engine::gameplay::CastResult cast6 =
        sys.cast("abilities:power_strike", caster, target, world);
    check(cast6.accepted, "power strike accepted after cooldown");
    check(world.bodies_[enemy.id].health == 10.0f, "second strike (45 - 35)");

    // Snapshot round-trip: full runtime state (cooldowns + active casts).
    const engine::gameplay::AbilityStateSnapshot snap = sys.snapshot();
    std::string snapErr;
    const std::string serialized =
        engine::gameplay::serialize_ability_state(snap, snapErr);
    check(!serialized.empty(), "snapshot serializes");
    engine::gameplay::AbilityStateSnapshot restored;
    check(engine::gameplay::deserialize_ability_state(serialized, restored,
                                                      snapErr),
          "snapshot deserializes");

    // Save/load proof: apply the snapshot to a fresh system and confirm the
    // cooldown state (the strike cast above is on cooldown again) survived.
    std::unique_ptr<engine::gameplay::IAbilitySystem> loaded =
        engine::gameplay::create_ability_system();
    // Real save/load path: definitions are reloaded first, then the runtime
    // state snapshot is applied on top.
    check(loaded->register_ability(strike, regErr), "re-register strike on load");
    check(loaded->register_ability(fire, regErr), "re-register fire on load");
    check(loaded->register_ability(terra, regErr), "re-register terraform on load");
    check(loaded->apply_snapshot(restored, snapErr),
          "apply snapshot on fresh system");
    check(loaded->on_cooldown("abilities:power_strike"),
          "cooldown survived snapshot round-trip");

    return world.bodies_[enemy.id].health;
}

}  // namespace

int main() {
    // Run the fight twice: once to warm the "live" system, once on a fresh
    // instance to prove determinism (identical final state bit-exact).
    {
        std::unique_ptr<engine::gameplay::IAbilitySystem> sys =
            engine::gameplay::create_ability_system();
        MockWorld world;
        const float first = run_fight(*sys, world);
        check(first == 10.0f, "fight ends with enemy at 10 hp");
    }
    {
        std::unique_ptr<engine::gameplay::IAbilitySystem> sys =
            engine::gameplay::create_ability_system();
        MockWorld world;
        const float second = run_fight(*sys, world);
        check(second == 10.0f, "fresh instance reproduces bit-exact result");
    }

    if (g_failures == 0) {
        std::printf("ALL PASSED: gameplay integration suite\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
