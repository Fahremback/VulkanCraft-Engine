#pragma once

// Public mob behavior (SDK, META section 13 fluid damage / FALTANTES item 11).
// The legacy Mob/MobManager track (coupled to World/SoundEngine/renderer) is
// removed: mobs are IEntityWorld entities carrying a versioned mob component
// (a JSON MobSpec blob under kMobComponentType). IMobBehavior advances them
// deterministically — gravity, fluid damage from the fluid's damagePerTick,
// fluids are never ground (entities sink through them), solid ground rest,
// simple idle/wander/chase AI toward the player, walk animation, and
// despawn on death. World access is a minimal IMobWorldQuery so both the SDK
// voxel world and a game project's own world can host mobs. This header is
// self-contained and never leaks external types.

#include <cstdint>
#include <memory>
#include <string>

#include "engine/entity/IEntityWorld.hpp"

namespace engine {
namespace entity {

// Minimal world queries a mob behavior needs (META section 13). The SDK's
// voxel world satisfies this (VoxelWorldFacade implements it); a game project
// adapts its own world — the entity layer never couples to the simulation
// World or the renderer.
class IMobWorldQuery {
public:
    virtual ~IMobWorldQuery() = default;

    // Block id at the cell (0 = air; engine/public block ids).
    virtual uint32_t block_at(int x, int y, int z) const = 0;

    // True when the cell holds a fluid (water, lava or a data-driven fluid).
    virtual bool is_fluid_block_at(int x, int y, int z) const = 0;

    // Per-second damage of the fluid occupying the cell (the definition's
    // damagePerTick); 0 when the cell has no fluid or a harmless one.
    // Deterministic — drives entity effects (META section 13).
    virtual float fluid_damage_per_second_at(int x, int y, int z) const = 0;
};

// Authored mob behavior. The mob component blob (kMobComponentType) is a JSON
// document: {"typeIndex":N,"maxHealth":H,"speed":S,"chaseSpeed":C,
// "chaseRange":R,"yaw":..,"walkAnimProgress":..,"fuseTimer":..}. Health lives
// in the builtin Health component; position in the builtin Position component.
struct MobSpec {
    uint32_t typeIndex{ 3 };    // renderer limb set (0..5), legacy MobType order
    float maxHealth{ 10.0f };
    float speed{ 1.8f };        // idle/wander speed
    float chaseSpeed{ 3.2f };   // speed toward the player inside chaseRange
    float chaseRange{ 16.0f };  // player distance that triggers the chase
    bool hostile{ false };      // chases the player inside chaseRange
    float yaw{ 0.0f };          // facing (renderer)
    float walkAnimProgress{ 0.0f };
    float fuseTimer{ 0.0f };    // creeper-style swell (renderer)
};

// Component type id of the mob behavior. Projects attach a component of this
// type (version 1, JSON blob above) to an IEntityWorld entity to make it a mob.
inline constexpr const char* kMobComponentType = "project:mob";

class IMobBehavior {
public:
    virtual ~IMobBehavior() = default;

    // Advances every entity carrying the kMobComponentType component:
    // gravity, fluid damage (the fluid's damagePerTick at the entity's feet),
    // fluids are never ground (sinking), solid ground rest, idle/wander/chase
    // AI toward the player, walk animation, and despawn when health <= 0.
    // Deterministic: wander decisions derive from the entity id (splitmix64),
    // so a fixed population and tick sequence reproduce bit-exactly.
    virtual bool tick(float dt, const Position& playerPos,
                      IEntityWorld& entities, const IMobWorldQuery& world,
                      std::string& errorOut) = 0;
};

// Serializes a mob component blob (the JSON document under kMobComponentType)
// from a spec with default runtime state. Projects use it when spawning mobs;
// the behavior rewrites the document each tick (runtime fields ride along).
std::string serialize_mob_spec(const MobSpec& spec);

// Deterministic implementation (the only TU with the behavior).
std::unique_ptr<IMobBehavior> create_mob_behavior();

}  // namespace entity
}  // namespace engine
