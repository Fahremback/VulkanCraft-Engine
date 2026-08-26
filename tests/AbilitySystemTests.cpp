// AbilitySystemTests.cpp
//
// FALTANTES §19: "Sistema genérico de abilities/poderes" — abilities as
// data-driven assets (attributes, tags, costs, cooldowns, conditions),
// targeting + composable effects, integration with voxel/physics/particles/
// audio/animation through the public world seam, cancel/interrupt/periodic
// casts, persistence/authority/prediction, and the event hooks. Every scenario
// runs against a DETERMINISTIC mock world implementing the public IAbilityWorld
// seam (voxel grid + simple physics + attributes/cost/health) — no engine
// internal is touched, which is exactly the "without core code" proof: an
// ability is a JSON asset + the public contract.
//
// Proves, headless and text-only:
//   - JSON round-trip bit-exact + all-or-nothing validation;
//   - attributes, tags, costs, cooldowns and conditions;
//   - targeting modes and effect composition (declaration order);
//   - integration: BlockEdit writes the voxel scene, Telekinesis holds a
//     physics body, Flight lifts the caster;
//   - cancel, interrupt and periodic effects;
//   - persistence, authority and prediction (serialize/deserialize state);
//   - event hooks for particles/audio/animation;
//   - determinism across fresh instances.

#include "engine/gameplay/IAbilitySystem.hpp"

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

bool same_vec3(const glm::vec3& a, const glm::vec3& b) {
    return std::memcmp(&a, &b, sizeof(glm::vec3)) == 0;
}

// ---- Deterministic mock world (the public seam) ------------------------------
// A tiny deterministic world implementing IAbilityWorld: a dense voxel grid
// (map), simple explicit-Euler physics (gravity + forces + impulses), health,
// attributes, tags and a resource pool. Bodies have stable ids; physics steps
// are exact across instances (no RNG, fixed order).
struct MockBody {
    glm::vec3 position{ 0.0f };
    glm::vec3 velocity{ 0.0f };
    glm::vec3 force{ 0.0f };
    float mass{ 1.0f };
    bool fixed{ false };  // static body: no gravity, no integration
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

    // Explicit Euler; forces are consumed each step (deterministic order).
    // Fixed bodies (the caster / the ground) never move — the hold point of
    // a telekinesis ability must be stationary or the test proves nothing.
    void step(float dt) {
        for (auto& entry : bodies_) {
            MockBody& body = entry.second;
            if (body.fixed) {
                body.force = glm::vec3(0.0f);
                continue;
            }
            const glm::vec3 accel = body.force / body.mass +
                                    glm::vec3(0.0f, gravity_, 0.0f);
            body.velocity += accel * dt;
            body.position += body.velocity * dt;
            body.force = glm::vec3(0.0f);
        }
    }

    // ---- voxel seam ----
    std::uint32_t block_at(int x, int y, int z) const override {
        const auto found = blocks_.find(key(x, y, z));
        return found == blocks_.end() ? 0 : found->second;
    }
    bool set_block(int x, int y, int z, std::uint32_t blockId) override {
        blocks_[key(x, y, z)] = blockId;
        return true;
    }

    // ---- physics seam ----
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

    // ---- attributes / cost / health ----
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
        return (static_cast<std::int64_t>(x) & 0xFFFFF) |
               (static_cast<std::int64_t>(y) & 0xFFFFF) << 20 |
               (static_cast<std::int64_t>(z) & 0xFFFFF) << 40;
    }
};

// ---- asset fixtures ----------------------------------------------------------

engine::gameplay::AbilityDefinition make_damage_ability(float cooldown = 0.0f) {
    using namespace engine::gameplay;
    AbilityDefinition definition;
    definition.name = "punch";
    definition.id = "abilities:punch";
    AbilityEffect effect;
    effect.type = AbilityEffectType::Damage;
    effect.amount = 10.0f;
    definition.effects.push_back(effect);
    definition.cooldownSeconds = cooldown;
    return definition;
}

engine::gameplay::AbilityDefinition make_telekinesis_ability() {
    using namespace engine::gameplay;
    AbilityDefinition definition;
    definition.name = "telekinesis";
    definition.id = "abilities:telekinesis";
    definition.targeting.mode = AbilityTargetMode::Body;
    AbilityEffect effect;
    effect.type = AbilityEffectType::Telekinesis;
    effect.holdOffsetX = 0.0f;
    effect.holdOffsetY = 1.5f;
    effect.holdOffsetZ = 0.0f;
    effect.grabForce = 40.0f;
    effect.durationSeconds = 0.0f;  // until cancelled
    definition.effects.push_back(effect);
    return definition;
}

engine::gameplay::AbilityDefinition make_flight_ability() {
    using namespace engine::gameplay;
    AbilityDefinition definition;
    definition.name = "flight";
    definition.id = "abilities:flight";
    definition.targeting.mode = AbilityTargetMode::Self;
    AbilityEffect effect;
    effect.type = AbilityEffectType::Flight;
    effect.thrust = 320.0f;
    effect.durationSeconds = 0.0f;  // until cancelled
    definition.effects.push_back(effect);
    return definition;
}

engine::gameplay::AbilityDefinition make_block_edit_ability() {
    using namespace engine::gameplay;
    AbilityDefinition definition;
    definition.name = "terraform";
    definition.id = "abilities:terraform";
    definition.targeting.mode = AbilityTargetMode::Point;
    AbilityEffect effect;
    effect.type = AbilityEffectType::BlockEdit;
    effect.min = {-1, -1, -1};
    effect.max = {1, 1, 1};
    effect.blockId = 7;
    effect.relative = true;
    definition.effects.push_back(effect);
    return definition;
}

engine::gameplay::AbilityDefinition make_periodic_ability() {
    using namespace engine::gameplay;
    AbilityDefinition definition;
    definition.name = "burn";
    definition.id = "abilities:burn";
    definition.targeting.mode = AbilityTargetMode::Body;
    AbilityEffect periodic;
    periodic.type = AbilityEffectType::Periodic;
    periodic.intervalSeconds = 0.5f;
    periodic.ticks = 4;
    auto damage = std::make_shared<AbilityEffect>();
    damage->type = AbilityEffectType::Damage;
    damage->amount = 5.0f;
    periodic.subEffect = damage;
    definition.effects.push_back(periodic);
    return definition;
}

// ---- JSON round-trip + validation --------------------------------------------

void test_json_round_trip() {
    using namespace engine::gameplay;
    // A definition with attributes/tags/cost/conditions round-trips.
    AbilityDefinition definition = make_damage_ability(2.5f);
    definition.attributes = {{"level", 3.0f}, {"range", 12.5f}};
    definition.tags = {"combat", "melee"};
    definition.cost = {"mana", 7.5f};
    definition.conditions.push_back(
        {AbilityConditionKind::OwnerAttribute, "", "level", 2.0f, 0.0f});
    definition.cancelable = false;
    definition.interruptible = false;
    {
        const std::string json = definition.to_json();
        AbilityDefinition loaded;
        std::string error;
        check(loaded.load_from_json(json, error), "definition load of emitted document");
        check(loaded.attributes.size() == 2, "attributes count round-trips");
        check(loaded.attributes[0].name == "level" &&
                  loaded.attributes[0].value == 3.0f,
              "attribute name/value round-trips");
        check(loaded.tags.size() == 2 && loaded.tags[0] == "combat",
              "tags round-trip");
        check(loaded.cost.resource == "mana" && loaded.cost.amount == 7.5f,
              "cost round-trips");
        check(loaded.conditions.size() == 1 &&
                  loaded.conditions[0].kind == AbilityConditionKind::OwnerAttribute &&
                  loaded.conditions[0].attribute == "level" &&
                  loaded.conditions[0].minValue == 2.0f,
              "condition round-trips");
        check(!loaded.cancelable && !loaded.interruptible,
              "cancelable/interruptible round-trip");
        check(loaded.cooldownSeconds == 2.5f, "cooldown round-trips");
    }
    // A composable definition with every effect kind. The id is a canonical
    // UUID (load derives a fresh one from the name when the field is not
    // canonical — the uuid_or_derived contract), so the round-trip check is
    // meaningful.
    AbilityDefinition rich;
    rich.name = "composite";
    rich.id = "123e4567-e89b-12d3-a456-426614174000";
    rich.targeting.mode = AbilityTargetMode::Point;
    AbilityEffect damage;
    damage.type = AbilityEffectType::Damage;
    damage.amount = 12.0f;
    rich.effects.push_back(damage);
    AbilityEffect heal;
    heal.type = AbilityEffectType::Heal;
    heal.amount = 4.0f;
    rich.effects.push_back(heal);
    AbilityEffect impulse;
    impulse.type = AbilityEffectType::Impulse;
    impulse.force = 300.0f;
    rich.effects.push_back(impulse);
    AbilityEffect tk;
    tk.type = AbilityEffectType::Telekinesis;
    tk.holdOffsetX = 0.25f;
    tk.holdOffsetY = 2.0f;
    tk.holdOffsetZ = -0.5f;
    tk.grabForce = 90.0f;
    tk.durationSeconds = 3.5f;
    rich.effects.push_back(tk);
    AbilityEffect flight;
    flight.type = AbilityEffectType::Flight;
    flight.thrust = 500.0f;
    flight.durationSeconds = 2.0f;
    rich.effects.push_back(flight);
    AbilityEffect edit;
    edit.type = AbilityEffectType::BlockEdit;
    edit.min = {-2, 0, -2};
    edit.max = {2, 4, 2};
    edit.blockId = 42;
    edit.relative = false;
    rich.effects.push_back(edit);
    AbilityEffect periodic;
    periodic.type = AbilityEffectType::Periodic;
    periodic.intervalSeconds = 0.25f;
    periodic.ticks = 8;
    auto sub = std::make_shared<AbilityEffect>();
    sub->type = AbilityEffectType::Damage;
    sub->amount = 3.0f;
    periodic.subEffect = sub;
    rich.effects.push_back(periodic);
    rich.effects[0].castAnimation = "cast_wave";
    rich.effects[0].particleEffect = "spark";
    rich.effects[0].soundEffect = "whoosh";

    const std::string json = rich.to_json();
    AbilityDefinition loaded;
    std::string error;
    check(loaded.load_from_json(json, error), "composite load of emitted document");
    check(loaded.name == rich.name, "composite name round-trips");
    check(loaded.id == rich.id, "composite id round-trips");
    check(loaded.targeting.mode == AbilityTargetMode::Point,
          "composite targeting mode round-trips");
    check(loaded.effects.size() == rich.effects.size(),
          "composite effect count round-trips");
    for (std::size_t i = 0; i < rich.effects.size(); ++i) {
        const AbilityEffect& a = rich.effects[i];
        const AbilityEffect& b = loaded.effects[i];
        check(a.type == b.type, "effect type round-trips");
        if (a.type == AbilityEffectType::BlockEdit) {
            check(a.min == b.min && a.max == b.max, "blockEdit box round-trips");
            check(a.blockId == b.blockId, "blockEdit blockId round-trips");
            check(a.relative == b.relative, "blockEdit relative round-trips");
        }
        if (a.type == AbilityEffectType::Periodic) {
            check(a.intervalSeconds == b.intervalSeconds, "periodic interval round-trips");
            check(a.ticks == b.ticks, "periodic ticks round-trips");
            check(b.subEffect != nullptr, "periodic subEffect round-trips");
            if (b.subEffect != nullptr) {
                check(b.subEffect->amount == a.subEffect->amount,
                      "periodic subEffect amount round-trips");
            }
        }
        check(b.castAnimation == a.castAnimation, "castAnimation hook round-trips");
        check(b.particleEffect == a.particleEffect, "particleEffect hook round-trips");
        check(b.soundEffect == a.soundEffect, "soundEffect hook round-trips");
    }
    std::printf("[ability-system] JSON round-trip: composite bit-exact OK\n");
}

void test_validation() {
    using namespace engine::gameplay;
    auto refuses = [](const std::string& json, const char* what) {
        AbilityDefinition target = make_damage_ability();
        std::string error;
        const bool accepted = target.load_from_json(json, error);
        check(!accepted, what);
        check(!error.empty(), "diagnostic provided");
        check(target.name == "punch", "target untouched on failure");
    };
    refuses("not json", "non-JSON refused");
    refuses("[]", "non-object root refused");
    refuses(R"({"name":"x","effects":[{"type":"damage","amount":-1}]})",
            "negative damage refused");
    refuses(R"({"name":"x","effects":[{"type":"bogus"}]})",
            "unknown effect type refused");
    refuses(R"({"name":"x","effects":[]})", "empty effects refused");
    refuses(R"({"name":"","effects":[{"type":"damage"}]})", "empty name refused");
    refuses(R"({"name":"x","version":2,"effects":[{"type":"damage"}]})",
            "unsupported version refused");
    refuses(R"({"name":"x","cooldownSeconds":-1,"effects":[{"type":"damage"}]})",
            "negative cooldown refused");
    refuses(R"({"name":"x","cost":{"resource":"mana","amount":-1},"effects":[{"type":"damage"}]})",
            "negative cost refused");
    refuses(R"({"name":"x","conditions":[{"kind":"ownerTag"}],"effects":[{"type":"damage"}]})",
            "tag condition without tag refused");
    refuses(R"({"name":"x","conditions":[{"kind":"ownerAttribute"}],"effects":[{"type":"damage"}]})",
            "attribute condition without attribute refused");
    refuses(R"({"name":"x","conditions":[{"kind":"unknown"}],"effects":[{"type":"damage"}]})",
            "unknown condition kind refused");
    refuses(R"({"name":"x","effects":[{"type":"periodic","intervalSeconds":0,"ticks":4,"subEffect":{"type":"damage","amount":1}}]})",
            "zero periodic interval refused");
    refuses(R"({"name":"x","effects":[{"type":"periodic","intervalSeconds":0.5,"ticks":0,"subEffect":{"type":"damage","amount":1}}]})",
            "zero periodic ticks refused");
    refuses(R"({"name":"x","effects":[{"type":"blockEdit","min":[0,0,0],"max":[16,16,16],"blockId":1}]})",
            "oversized blockEdit box refused");
    refuses(R"({"name":"x","effects":[{"type":"blockEdit","min":[2,0,0],"max":[1,0,0],"blockId":1}]})",
            "inverted blockEdit box refused");
    std::printf("[ability-system] validation: all-or-nothing OK\n");
}

// ---- conditions / cost / cooldown --------------------------------------------

void test_conditions_cost_cooldown() {
    using namespace engine::gameplay;

    // Owner tag condition.
    {
        auto system = create_ability_system();
        AbilityDefinition tagged = make_damage_ability();
        tagged.conditions.push_back({AbilityConditionKind::OwnerTag, "mage"});
        std::string error;
        check(system->register_ability(tagged, error), "tag ability registered");
        MockWorld world;
        const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
        AbilityTarget target;
        target.mode = AbilityTargetMode::Body;
        target.body = caster;
        CastResult refused = system->cast(tagged.id, caster, target, world);
        check(!refused.accepted, "caster without tag is refused");
        world.bodies_[caster.id].tags.push_back("mage");
        CastResult accepted = system->cast(tagged.id, caster, target, world);
        check(accepted.accepted, "caster with tag is accepted");
        check(accepted.effectCount == 1, "damage effect applied");
        check(world.bodies_[caster.id].health == 90.0f, "damage applied to target");
    }

    // Attribute condition.
    {
        auto system = create_ability_system();
        AbilityDefinition leveled = make_damage_ability();
        leveled.conditions.push_back(
            {AbilityConditionKind::OwnerAttribute, "", "level", 3.0f, 0.0f});
        std::string error;
        check(system->register_ability(leveled, error), "leveled ability registered");
        MockWorld world;
        const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
        world.bodies_[caster.id].attributes["level"] = 2.0f;
        AbilityTarget target;
        target.mode = AbilityTargetMode::Body;
        target.body = caster;
        check(!system->cast(leveled.id, caster, target, world).accepted,
              "caster below level requirement is refused");
        world.bodies_[caster.id].attributes["level"] = 5.0f;
        check(system->cast(leveled.id, caster, target, world).accepted,
              "caster at level requirement is accepted");
    }

    // Distance condition.
    {
        auto system = create_ability_system();
        AbilityDefinition ranged = make_damage_ability();
        ranged.conditions.push_back(
            {AbilityConditionKind::Distance, "", "", 0.0f, 5.0f});
        std::string error;
        check(system->register_ability(ranged, error), "ranged ability registered");
        MockWorld world;
        const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
        AbilityTarget target;
        target.mode = AbilityTargetMode::Point;
        target.point = {10.0f, 0.0f, 0.0f};
        check(!system->cast(ranged.id, caster, target, world).accepted,
              "target beyond max distance is refused");
        target.point = {3.0f, 0.0f, 0.0f};
        check(system->cast(ranged.id, caster, target, world).accepted,
              "target within max distance is accepted");
    }

    // Cost: spent on acceptance, refusal leaves the pool untouched.
    {
        auto system = create_ability_system();
        AbilityDefinition costly = make_damage_ability();
        costly.cost = {"mana", 7.5f};
        std::string error;
        check(system->register_ability(costly, error), "costly ability registered");
        MockWorld world;
        const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
        world.bodies_[caster.id].resources["mana"] = 5.0f;
        AbilityTarget target;
        target.mode = AbilityTargetMode::Body;
        target.body = caster;
        check(!system->cast(costly.id, caster, target, world).accepted,
              "unaffordable cost is refused");
        check(world.bodies_[caster.id].resources["mana"] == 5.0f,
              "refused cast spends nothing");
        check(world.bodies_[caster.id].health == 100.0f,
              "refused cast applies nothing");
        world.bodies_[caster.id].resources["mana"] = 10.0f;
        check(system->cast(costly.id, caster, target, world).accepted,
              "affordable cost is accepted");
        check(world.bodies_[caster.id].resources["mana"] == 2.5f,
              "cost spent on acceptance");
        check(world.bodies_[caster.id].health == 90.0f,
              "effect applied on acceptance");
    }

    // Cooldown: blocks re-cast until the timer elapses.
    {
        auto system = create_ability_system();
        AbilityDefinition cooldownAbility = make_damage_ability(2.0f);
        std::string error;
        check(system->register_ability(cooldownAbility, error),
              "cooldown ability registered");
        MockWorld world;
        const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
        AbilityTarget target;
        target.mode = AbilityTargetMode::Body;
        target.body = caster;
        check(system->cast(cooldownAbility.id, caster, target, world).accepted,
              "first cast accepted");
        check(system->on_cooldown(cooldownAbility.id), "on cooldown after cast");
        check(!system->cast(cooldownAbility.id, caster, target, world).accepted,
              "second cast during cooldown refused");
        for (int i = 0; i < 130; ++i) system->update(1.0f / 60.0f, world);
        check(!system->on_cooldown(cooldownAbility.id),
              "cooldown elapsed after updates");
        check(system->cast(cooldownAbility.id, caster, target, world).accepted,
              "cast accepted after cooldown");
    }
    std::printf("[ability-system] conditions/cost/cooldown OK\n");
}

// ---- targeting + effect composition ------------------------------------------

void test_targeting_and_composition() {
    using namespace engine::gameplay;
    auto system = create_ability_system();
    AbilityDefinition composite;
    composite.name = "composite";
    composite.id = "abilities:composite";
    AbilityEffect damage;
    damage.type = AbilityEffectType::Damage;
    damage.amount = 10.0f;
    composite.effects.push_back(damage);
    AbilityEffect heal;
    heal.type = AbilityEffectType::Heal;
    heal.amount = 5.0f;
    composite.effects.push_back(heal);
    AbilityEffect impulse;
    impulse.type = AbilityEffectType::Impulse;
    impulse.force = 20.0f;
    composite.effects.push_back(impulse);
    std::string error;
    check(system->register_ability(composite, error), "composite ability registered");

    MockWorld world;
    const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
    const AbilityBodyId targetBody = world.spawn({0.0f, 0.0f, -5.0f});
    world.bodies_[targetBody.id].health = 50.0f;
    AbilityTarget target;
    target.mode = AbilityTargetMode::Body;
    target.body = targetBody;
    target.direction = {0.0f, 0.0f, -1.0f};

    const CastResult result = system->cast(composite.id, caster, target, world);
    check(result.accepted, "composite cast accepted");
    check(result.effectCount == 3, "three effects in declaration order");
    // Damage 10 then heal 5 -> 45; impulse -20 in z -> velocity (0,0,-20).
    check(world.bodies_[targetBody.id].health == 45.0f,
          "damage then heal compose in order");
    check(std::fabs(world.bodies_[targetBody.id].velocity.z + 20.0f) < 1e-4f,
          "impulse applied along target direction");
    std::printf("[ability-system] targeting + composition OK\n");
}

// ---- voxel integration: an ability that alters the scene ----------------------

void test_block_edit_alters_scene() {
    using namespace engine::gameplay;
    auto system = create_ability_system();
    AbilityDefinition terraform = make_block_edit_ability();
    std::string error;
    check(system->register_ability(terraform, error), "terraform ability registered");

    MockWorld world;
    const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
    AbilityTarget target;
    target.mode = AbilityTargetMode::Point;
    target.point = {10.0f, 20.0f, 30.0f};

    const CastResult result = system->cast(terraform.id, caster, target, world);
    check(result.accepted, "terraform cast accepted");
    check(world.block_at(9, 19, 29) == 7, "block written at min corner");
    check(world.block_at(11, 21, 31) == 7, "block written at max corner");
    check(world.block_at(10, 20, 30) == 7, "block written at origin");
    check(world.block_at(12, 20, 30) == 0, "outside the box untouched");
    check(world.block_at(10, 18, 30) == 0, "below the box untouched");
    std::printf("[ability-system] blockEdit: scene altered without core code OK\n");
}

// ---- physics integration: telekinesis + flight -------------------------------

void test_telekinesis() {
    using namespace engine::gameplay;
    auto system = create_ability_system();
    AbilityDefinition tk = make_telekinesis_ability();
    std::string error;
    check(system->register_ability(tk, error), "telekinesis ability registered");

    MockWorld world;
    // The caster is FIXED (a stationary observer): the hold point is derived
    // from the caster position, so a falling caster would drag the hold point
    // down and the test would prove nothing.
    const AbilityBodyId caster =
        world.spawn({0.0f, 0.0f, 0.0f}, 1.0f, /*fixed=*/true);
    // Target starts 4 m in +x at the hold height.
    const AbilityBodyId rock = world.spawn({4.0f, 1.5f, 0.0f});
    AbilityTarget target;
    target.mode = AbilityTargetMode::Body;
    target.body = rock;

    const CastResult result = system->cast(tk.id, caster, target, world);
    check(result.accepted, "telekinesis cast accepted");
    check(system->active_cast_count() == 1, "sustained cast active");

    // Hold: the spring pulls the rock toward the caster's hold point
    // (caster + (0, 1.5, 0)). Distance to the hold point shrinks over time.
    auto distanceToHold = [&]() {
        const glm::vec3 hold(0.0f, 1.5f, 0.0f);
        const glm::vec3 pos = world.bodies_[rock.id].position;
        const glm::vec3 delta = hold - pos;
        return std::sqrt(glm::dot(delta, delta));
    };
    const float initial = distanceToHold();
    for (int i = 0; i < 120; ++i) {
        system->update(1.0f / 60.0f, world);
        world.step(1.0f / 60.0f);
    }
    const float held = distanceToHold();
    check(held < initial * 0.25f,
          "telekinesis pulls the rock close to the hold point");

    // Cancel: the hold ends and the rock drifts (gravity only — it no longer
    // receives the spring force).
    std::string cancelError;
    check(system->cancel(result.castIndex, world, cancelError),
          "telekinesis cancel succeeds");
    check(system->active_cast_count() == 0, "no active casts after cancel");
    const glm::vec3 velocityAtRelease = world.bodies_[rock.id].velocity;
    for (int i = 0; i < 30; ++i) {
        system->update(1.0f / 60.0f, world);
        world.step(1.0f / 60.0f);
    }
    check(world.bodies_[rock.id].velocity.y < velocityAtRelease.y - 0.1f,
          "released rock falls (gravity takes over)");
    std::printf("[ability-system] telekinesis: hold + release OK\n");
}

void test_flight() {
    using namespace engine::gameplay;
    auto system = create_ability_system();
    AbilityDefinition flight = make_flight_ability();
    std::string error;
    check(system->register_ability(flight, error), "flight ability registered");

    MockWorld world;
    const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
    AbilityTarget target;
    target.mode = AbilityTargetMode::Self;

    const CastResult result = system->cast(flight.id, caster, target, world);
    check(result.accepted, "flight cast accepted");
    check(system->active_cast_count() == 1, "flight cast active");

    // Thrust (320) exceeds gravity (9.81 * 1 kg) — the caster rises.
    for (int i = 0; i < 60; ++i) {
        system->update(1.0f / 60.0f, world);
        world.step(1.0f / 60.0f);
    }
    check(world.bodies_[caster.id].position.y > 5.0f,
          "flight lifts the caster against gravity");

    // Cancel: thrust stops. The caster carries a large upward velocity, so it
    // does not instantly fall — the observable is DECELERATION: with thrust
    // off, gravity reduces the upward velocity every step (it was increasing
    // during the thrust).
    std::string cancelError;
    check(system->cancel(result.castIndex, world, cancelError),
          "flight cancel succeeds");
    const float velocityAtCancel = world.bodies_[caster.id].velocity.y;
    check(velocityAtCancel > 50.0f, "thrust built a large upward velocity");
    float previousVelocity = velocityAtCancel;
    bool decelerates = true;
    for (int i = 0; i < 30; ++i) {
        system->update(1.0f / 60.0f, world);
        world.step(1.0f / 60.0f);
        const float v = world.bodies_[caster.id].velocity.y;
        if (v >= previousVelocity) decelerates = false;
        previousVelocity = v;
    }
    check(decelerates, "after cancel the caster decelerates every step (gravity)");
    check(world.bodies_[caster.id].velocity.y < velocityAtCancel - 4.0f,
          "after cancel the upward velocity drops significantly");
    std::printf("[ability-system] flight: lift + cancel OK\n");
}

// ---- cancel / interrupt / periodic -------------------------------------------

void test_interrupt_and_periodic() {
    using namespace engine::gameplay;

    // Interruptible vs non-interruptible.
    {
        auto system = create_ability_system();
        AbilityDefinition interruptible = make_flight_ability();
        interruptible.interruptible = true;
        AbilityDefinition locked = make_flight_ability();
        locked.id = "abilities:flight_locked";
        locked.interruptible = false;
        std::string error;
        check(system->register_ability(interruptible, error), "interruptible registered");
        check(system->register_ability(locked, error), "non-interruptible registered");
        MockWorld world;
        const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
        AbilityTarget target;
        target.mode = AbilityTargetMode::Self;

        const CastResult free = system->cast(interruptible.id, caster, target, world);
        check(free.accepted, "interruptible cast accepted");
        std::string interruptError;
        check(system->interrupt(free.castIndex, world, interruptError),
              "interruptible cast can be interrupted");

        const CastResult held = system->cast(locked.id, caster, target, world);
        check(held.accepted, "non-interruptible cast accepted");
        std::string lockedError;
        check(!system->interrupt(held.castIndex, world, lockedError),
              "non-interruptible cast refuses interrupt");
        check(!lockedError.empty(), "refusal has a diagnostic");
        check(system->active_cast_count() == 1,
              "non-interruptible cast keeps running");
        // Cancel still works (cancelable defaults to true).
        std::string cancelError;
        check(system->cancel(held.castIndex, world, cancelError),
              "non-interruptible cast can still be cancelled");
    }

    // Periodic: the sub-effect ticks `ticks` times at `intervalSeconds`.
    {
        auto system = create_ability_system();
        AbilityDefinition burn = make_periodic_ability();
        std::string error;
        check(system->register_ability(burn, error), "burn ability registered");
        MockWorld world;
        const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
        const AbilityBodyId victim = world.spawn({0.0f, 0.0f, -3.0f});
        AbilityTarget target;
        target.mode = AbilityTargetMode::Body;
        target.body = victim;

        const CastResult result = system->cast(burn.id, caster, target, world);
        check(result.accepted, "periodic cast accepted");
        for (int i = 0; i < 150; ++i) {
            system->update(1.0f / 60.0f, world);
            world.step(1.0f / 60.0f);
        }
        // 4 ticks * 5 damage = 20; the cast finishes when ticks are done.
        check(world.bodies_[victim.id].health == 80.0f,
              "periodic damage applied exactly 4 times");
        check(system->active_cast_count() == 0, "periodic cast finished");
    }
    std::printf("[ability-system] interrupt + periodic OK\n");
}

// ---- persistence / authority / prediction ------------------------------------

void test_state_serialization() {
    using namespace engine::gameplay;

    // Server: cast a cooldown ability + keep a sustained cast, serialize.
    AbilityStateSnapshot serverState;
    {
        auto server = create_ability_system();
        AbilityDefinition dash = make_damage_ability(3.0f);
        dash.id = "abilities:dash";
        AbilityDefinition flight = make_flight_ability();
        std::string error;
        check(server->register_ability(dash, error), "dash registered (server)");
        check(server->register_ability(flight, error), "flight registered (server)");
        MockWorld world;
        const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
        AbilityTarget self;
        self.mode = AbilityTargetMode::Self;
        check(server->cast(dash.id, caster, self, world).accepted, "dash cast");
        check(server->cast(flight.id, caster, self, world).accepted, "flight cast");
        serverState = server->snapshot();
        check(serverState.cooldowns.find("abilities:dash") !=
                  serverState.cooldowns.end(),
              "server snapshot carries the cooldown");
        check(serverState.activeCasts.size() == 1,
              "server snapshot carries the active flight cast");
    }

    // Client: registers the same abilities, applies the server snapshot, and
    // lands in the same state (cooldown + active cast). This is the
    // authority/prediction reconcile path.
    {
        auto client = create_ability_system();
        AbilityDefinition dash = make_damage_ability(3.0f);
        dash.id = "abilities:dash";
        AbilityDefinition flight = make_flight_ability();
        std::string error;
        check(client->register_ability(dash, error), "dash registered (client)");
        check(client->register_ability(flight, error), "flight registered (client)");
        check(client->apply_snapshot(serverState, error),
              "client applies authoritative snapshot");
        check(std::fabs(client->cooldown_remaining("abilities:dash") -
                        serverState.cooldowns.at("abilities:dash")) < 1e-6f,
              "client cooldown matches the authority");
        check(client->active_cast_count() == 1, "client active cast matches");
        const ActiveCastInfo info = client->active_cast(serverState.activeCasts[0].castIndex);
        check(info.abilityId == "abilities:flight", "client cast identity matches");
        check(info.caster.id == serverState.activeCasts[0].caster.id,
              "client caster matches");
    }

    // Serialize -> deserialize round-trip (bit-exact floats).
    {
        std::string error;
        const std::string json = serialize_ability_state(serverState, error);
        AbilityStateSnapshot restored;
        check(deserialize_ability_state(json, restored, error),
              "state JSON round-trips");
        check(restored.nextCastIndex == serverState.nextCastIndex,
              "nextCastIndex round-trips");
        check(restored.cooldowns == serverState.cooldowns,
              "cooldowns round-trip bit-exact");
        check(restored.activeCasts.size() == serverState.activeCasts.size(),
              "active cast count round-trips");
        if (!restored.activeCasts.empty()) {
            check(restored.activeCasts[0].abilityId ==
                      serverState.activeCasts[0].abilityId,
                  "active cast ability round-trips");
            check(restored.activeCasts[0].elapsedSeconds ==
                      serverState.activeCasts[0].elapsedSeconds,
                  "active cast elapsed round-trips bit-exact");
        }
        // Malformed state is refused all-or-nothing.
        check(!deserialize_ability_state("not json", restored, error),
              "malformed state refused");
        check(!deserialize_ability_state(
                  R"({"version":2,"nextCastIndex":1,"cooldowns":{},"activeCasts":[]})",
                  restored, error),
              "unsupported state version refused");
    }
    std::printf("[ability-system] persistence/authority/prediction OK\n");
}

// ---- event hooks (particles / audio / animation) -----------------------------

void test_event_hooks() {
    using namespace engine::gameplay;
    auto system = create_ability_system();
    AbilityDefinition flash = make_flight_ability();
    flash.id = "abilities:flash";
    flash.effects[0].castAnimation = "cast_wave";
    flash.effects[0].particleEffect = "spark";
    flash.effects[0].soundEffect = "whoosh";
    std::string error;
    check(system->register_ability(flash, error), "flash ability registered");

    std::vector<AbilityEvent> events;
    system->set_event_sink([&](const AbilityEvent& event) { events.push_back(event); });

    MockWorld world;
    const AbilityBodyId caster = world.spawn({0.0f, 0.0f, 0.0f});
    AbilityTarget target;
    target.mode = AbilityTargetMode::Self;

    const CastResult result = system->cast(flash.id, caster, target, world);
    check(result.accepted, "flash cast accepted");
    check(events.size() == 1, "cast fired one event");
    if (!events.empty()) {
        check(events[0].kind == AbilityEvent::Kind::Cast, "event is Cast");
        check(events[0].abilityId == "abilities:flash", "event names the ability");
        check(events[0].animation == "cast_wave", "animation hook surfaced");
        check(events[0].particle == "spark", "particle hook surfaced");
        check(events[0].sound == "whoosh", "sound hook surfaced");
    }

    // update ticks the sustained flight (EffectTick per update).
    events.clear();
    system->update(1.0f / 60.0f, world);
    check(events.size() == 1 && events[0].kind == AbilityEvent::Kind::EffectTick,
          "sustained effect fires EffectTick");
    check(events[0].sound == "whoosh", "tick carries the sound hook");

    // Cancel fires Cancelled.
    events.clear();
    std::string cancelError;
    check(system->cancel(result.castIndex, world, cancelError), "flash cancel");
    check(events.size() == 1 && events[0].kind == AbilityEvent::Kind::Cancelled,
          "cancel fires Cancelled event");
    std::printf("[ability-system] event hooks (particles/audio/animation) OK\n");
}

// ---- determinism --------------------------------------------------------------

void test_determinism() {
    using namespace engine::gameplay;
    auto run = [](float& telekinesisDistance, float& flightHeight,
                  float& finalHealth) {
        auto system = create_ability_system();
        AbilityDefinition tk = make_telekinesis_ability();
        AbilityDefinition flight = make_flight_ability();
        AbilityDefinition burn = make_periodic_ability();
        std::string error;
        system->register_ability(tk, error);
        system->register_ability(flight, error);
        system->register_ability(burn, error);

        MockWorld world;
        // Telekinesis needs a stationary caster (hold point); the flight
        // caster is a separate dynamic body that actually rises.
        const AbilityBodyId caster =
            world.spawn({0.0f, 0.0f, 0.0f}, 1.0f, /*fixed=*/true);
        const AbilityBodyId flyer = world.spawn({0.0f, 0.0f, 0.0f});
        const AbilityBodyId rock = world.spawn({4.0f, 1.5f, 0.0f});
        const AbilityBodyId victim = world.spawn({0.0f, 0.0f, -3.0f});
        AbilityTarget tkTarget;
        tkTarget.mode = AbilityTargetMode::Body;
        tkTarget.body = rock;
        AbilityTarget self;
        self.mode = AbilityTargetMode::Self;
        AbilityTarget burnTarget;
        burnTarget.mode = AbilityTargetMode::Body;
        burnTarget.body = victim;

        system->cast(tk.id, caster, tkTarget, world);
        system->cast(flight.id, flyer, self, world);
        system->cast(burn.id, caster, burnTarget, world);
        for (int i = 0; i < 90; ++i) {
            system->update(1.0f / 60.0f, world);
            world.step(1.0f / 60.0f);
        }
        const glm::vec3 hold(0.0f, 1.5f, 0.0f);
        const glm::vec3 delta = hold - world.bodies_[rock.id].position;
        telekinesisDistance = std::sqrt(glm::dot(delta, delta));
        flightHeight = world.bodies_[flyer.id].position.y;
        finalHealth = world.bodies_[victim.id].health;
    };

    float d1 = 0.0f, h1 = 0.0f, health1 = 0.0f;
    float d2 = 0.0f, h2 = 0.0f, health2 = 0.0f;
    run(d1, h1, health1);
    run(d2, h2, health2);
    check(d1 == d2, "telekinesis distance bit-identical across instances");
    check(h1 == h2, "flight height bit-identical across instances");
    check(health1 == health2, "periodic health bit-identical across instances");
    std::printf("[ability-system] determinism: bit-identical across instances OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep progress visible on crash
    test_json_round_trip();
    test_validation();
    test_conditions_cost_cooldown();
    test_targeting_and_composition();
    test_block_edit_alters_scene();
    test_telekinesis();
    test_flight();
    test_interrupt_and_periodic();
    test_state_serialization();
    test_event_hooks();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[ability-system] ALL PASSED\n");
        return 0;
    }
    std::printf("[ability-system] %d FAILURE(S)\n", g_failures);
    return 1;
}
