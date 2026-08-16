#include "engine/entity/IMobBehavior.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

// Mob behavior adapter (FALTANTES item 11): the only TU implementing the
// public IMobBehavior. Mobs are IEntityWorld entities carrying a versioned
// mob component (JSON MobSpec under kMobComponentType); the behavior advances
// them deterministically — gravity, fluid damage (the fluid's damagePerTick
// at the entity's body), fluids are never ground (entities sink through
// them), solid ground rest, idle/wander/chase AI toward the player, walk
// animation, and despawn on death. AI decisions derive from the entity id
// (splitmix64), so a fixed population and tick sequence reproduce bit-exactly
// across instances. No SoundEngine, no renderer, no core World coupling — the
// world access is the minimal IMobWorldQuery.

namespace engine {
namespace entity {
namespace {

// Deterministic per-entity RNG: decisions derive from the entity id, so the
// behavior reproduces bit-exactly for a fixed population.
struct SplitMix64 {
    explicit SplitMix64(uint64_t seed) : state_(seed) {}
    uint64_t next() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    float next_float01() {
        return static_cast<float>((next() >> 40) * (1.0 / 65536.0 / 65536.0));
    }

private:
    uint64_t state_{};
};

// Legacy Mob::update semantics (kept equivalent where it matters):
constexpr float kGravity = 18.0f;        // velocity.y -= gravity * dt
constexpr float kGroundProbe = 0.1f;     // solid probe below the feet
constexpr float kFluidProbe = 0.2f;      // fluid probe at the body
constexpr float kWalkAnimRate = 4.0f;    // walkAnimProgress gain per m/s
constexpr float kMaxTypeIndex = 5;       // renderer limb sets (0..5)
constexpr float kTwoPi = 6.28318530718f;

// Per-entity state that must persist across ticks, kept inside the component
// blob (versioned, saved with the world, renderer-visible).
struct MobRuntime {
    float velY{ 0.0f };
    float aiTimer{ 0.0f };
    bool wandering{ false };  // current non-chase decision: move along yaw
};

bool parse_spec(const std::string& blob, MobSpec& out) {
    sdk::JsonValue root;
    std::string error;
    if (!sdk::json_parse(blob, root, error) || !root.is_object()) return false;
    out.typeIndex = static_cast<uint32_t>(
        std::lround(sdk::json_number(root, "typeIndex", 3.0)));
    out.maxHealth = static_cast<float>(sdk::json_number(root, "maxHealth", 10.0));
    out.speed = static_cast<float>(sdk::json_number(root, "speed", 1.8));
    out.chaseSpeed = static_cast<float>(sdk::json_number(root, "chaseSpeed", 3.2));
    out.chaseRange = static_cast<float>(sdk::json_number(root, "chaseRange", 16.0));
    out.hostile = sdk::json_bool(root, "hostile", false);
    out.yaw = static_cast<float>(sdk::json_number(root, "yaw", 0.0));
    out.walkAnimProgress = static_cast<float>(
        sdk::json_number(root, "walkAnimProgress", 0.0));
    out.fuseTimer = static_cast<float>(sdk::json_number(root, "fuseTimer", 0.0));
    return true;
}

bool parse_runtime(const std::string& blob, MobRuntime& out) {
    sdk::JsonValue root;
    std::string error;
    if (!sdk::json_parse(blob, root, error) || !root.is_object()) return false;
    out.velY = static_cast<float>(sdk::json_number(root, "velY", 0.0));
    out.aiTimer = static_cast<float>(sdk::json_number(root, "aiTimer", 0.0));
    out.wandering = sdk::json_bool(root, "wandering", false);
    return true;
}

std::string serialize(const MobSpec& spec, const MobRuntime& rt) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"typeIndex\":" << spec.typeIndex << ",\"maxHealth\":"
        << spec.maxHealth << ",\"speed\":" << spec.speed << ",\"chaseSpeed\":"
        << spec.chaseSpeed << ",\"chaseRange\":" << spec.chaseRange
        << ",\"hostile\":" << (spec.hostile ? "true" : "false")
        << ",\"yaw\":" << spec.yaw << ",\"walkAnimProgress\":"
        << spec.walkAnimProgress << ",\"fuseTimer\":" << spec.fuseTimer
        << ",\"velY\":" << rt.velY << ",\"aiTimer\":" << rt.aiTimer
        << ",\"wandering\":" << (rt.wandering ? "true" : "false") << '}';
    return out.str();
}

}  // namespace

// Public: serializes a mob component blob from a spec (default runtime state;
// the behavior rewrites the document each tick).
std::string serialize_mob_spec(const MobSpec& spec) {
    return serialize(spec, MobRuntime{});
}

namespace {

class MobBehaviorImpl final : public IMobBehavior {
public:
    bool tick(float dt, const Position& playerPos, IEntityWorld& entities,
              const IMobWorldQuery& world, std::string& errorOut) override {
        if (!(dt >= 0.0f) || !std::isfinite(dt)) {
            errorOut = "mob behavior: invalid dt";
            return false;
        }

        // Pass 1 — validate every mob entity (all-or-nothing: a malformed mob
        // component or a missing health/position refuses the whole step with
        // no mutation, so a project bug never silently corrupts mobs).
        std::vector<EntityId> mobIds;
        bool valid = true;
        entities.for_each_entity([&](EntityId id) {
            ComponentData mob;
            if (!entities.get_component(id, kMobComponentType, mob)) return;
            MobSpec spec;
            if (!parse_spec(mob.blob, spec) ||
                spec.typeIndex > kMaxTypeIndex || spec.maxHealth <= 0.0f ||
                spec.speed < 0.0f || spec.chaseSpeed < 0.0f ||
                spec.chaseRange < 0.0f) {
                errorOut = "mob behavior: invalid mob component on entity " +
                           std::to_string(id.id);
                valid = false;
                return;
            }
            Health health;
            Position position;
            if (!entities.get_health(id, health) ||
                !entities.get_position(id, position)) {
                errorOut = "mob behavior: mob entity " + std::to_string(id.id) +
                           " is missing the health/position component";
                valid = false;
                return;
            }
            mobIds.push_back(id);
        });
        if (!valid) return false;

        // Pass 2 — advance (the world's own tick drives sleeping policies and
        // hands each entity its effective dt; component mutation inside the
        // callback is the documented project extension point).
        std::vector<EntityId> dead;
        entities.tick(dt, [&](EntityId id, float effectiveDt) {
            if (std::find(mobIds.begin(), mobIds.end(), id) == mobIds.end()) {
                return;
            }
            ComponentData mob;
            if (!entities.get_component(id, kMobComponentType, mob)) return;
            MobSpec spec;
            MobRuntime rt;
            if (!parse_spec(mob.blob, spec) || !parse_runtime(mob.blob, rt)) return;

            Health health;
            Position pos;
            if (!entities.get_health(id, health) ||
                !entities.get_position(id, pos)) {
                return;
            }

            // Fluid damage (META section 13): damagePerTick of the fluid at
            // the entity's body, per second.
            const float damage = world.fluid_damage_per_second_at(
                static_cast<int>(std::floor(pos.x)),
                static_cast<int>(std::floor(pos.y + kFluidProbe)),
                static_cast<int>(std::floor(pos.z)));
            if (damage > 0.0f) health.value -= damage * effectiveDt;

            // AI: hostile mobs chase the player inside chaseRange; everyone
            // else (and out-of-range hostiles) idle/wander on a timer whose
            // decisions derive from the entity id (deterministic).
            const float dx = playerPos.x - pos.x;
            const float dz = playerPos.z - pos.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            rt.aiTimer -= effectiveDt;
            float moveX = 0.0f;
            float moveZ = 0.0f;
            float speed = 0.0f;
            if (spec.hostile && dist < spec.chaseRange) {
                rt.wandering = false;
                if (dist > 0.01f) {
                    moveX = dx / dist;
                    moveZ = dz / dist;
                    spec.yaw = std::atan2(-moveX, -moveZ);
                }
                speed = spec.chaseSpeed;
            } else if (rt.aiTimer <= 0.0f) {
                // Re-decide: wander (2/3) or idle (1/3), 2..4 s cadence.
                SplitMix64 rng(static_cast<uint64_t>(id.id));
                rt.aiTimer = 2.0f + rng.next_float01() * 2.0f;
                rt.wandering = rng.next_float01() < (2.0f / 3.0f);
                if (rt.wandering) {
                    spec.yaw = rng.next_float01() * kTwoPi;
                    moveX = -std::sin(spec.yaw);
                    moveZ = -std::cos(spec.yaw);
                    speed = spec.speed;
                }
            } else if (rt.wandering) {
                moveX = -std::sin(spec.yaw);
                moveZ = -std::cos(spec.yaw);
                speed = spec.speed;
            }

            // Movement: gravity + horizontal AI velocity.
            rt.velY -= kGravity * effectiveDt;
            pos.x += moveX * speed * effectiveDt;
            pos.z += moveZ * speed * effectiveDt;
            pos.y += rt.velY * effectiveDt;
            spec.walkAnimProgress +=
                std::sqrt(moveX * moveX + moveZ * moveZ) * speed *
                effectiveDt * kWalkAnimRate;

            // Grounding: fluids are never ground (the entity sinks through
            // them to the solid floor); a solid block below snaps the entity
            // onto it.
            const int px = static_cast<int>(std::floor(pos.x));
            const int pz = static_cast<int>(std::floor(pos.z));
            const int below = static_cast<int>(std::floor(pos.y - kGroundProbe));
            if (!world.is_fluid_block_at(px, below, pz) &&
                world.block_at(px, below, pz) != 0) {
                pos.y = std::ceil(pos.y - kGroundProbe);
                rt.velY = 0.0f;
            }

            if (health.value <= 0.0f) {
                dead.push_back(id);
                return;  // do not write back a dead entity
            }

            entities.set_health(id, health);
            entities.set_position(id, pos);
            ComponentData next;
            next.type = kMobComponentType;
            next.version = 1;
            next.blob = serialize(spec, rt);
            entities.set_component(id, next);
        });

        // Pass 3 — despawn the dead (safe after the iteration).
        for (const EntityId& id : dead) entities.despawn(id);
        return true;
    }
};

}  // namespace

std::unique_ptr<IMobBehavior> create_mob_behavior() {
    return std::make_unique<MobBehaviorImpl>();
}

}  // namespace entity
}  // namespace engine
