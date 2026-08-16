#pragma once

// Public entity world (SDK, META section 15 / FALTANTES item 11). Entities are
// indexed by voxel chunk, carry generational handles (stale handles never
// alias a new entity), run headless (server/tests) with per-entity tick
// policies (sleeping), expose builtin components (health, position) plus
// project components as versioned opaque blobs, and persist through the world
// save (v5 `world_entities`). The default implementation is backed by EnTT
// (the ECS storage authority, DEPENDENCY_POLICY); this header is self-contained
// and never leaks external types.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace entity {

// Generational handle: `id` + `generation`. 0/null is the invalid handle.
// After a despawn the generation bumps, so a stale handle never aliases the
// entity that reuses the id (pooling).
struct EntityId {
    uint32_t id{ 0 };
    uint32_t generation{ 0 };

    bool valid() const { return id != 0; }
    bool operator==(const EntityId& other) const {
        return id == other.id && generation == other.generation;
    }
    bool operator!=(const EntityId& other) const { return !(*this == other); }
};

struct Position {
    float x{ 0.0f };
    float y{ 0.0f };
    float z{ 0.0f };
};

struct Health {
    float value{ 20.0f };
    float max{ 20.0f };
};

// Project-owned component: a registered type id plus a versioned opaque blob.
// Semantics belong to the project; the engine stores, versions and persists.
struct ComponentData {
    std::string type;   // registered component type id (project namespace)
    uint32_t version{ 1 };
    std::string blob;   // opaque payload
};

// Serialized form of one entity — the persistence/replication unit.
struct EntitySnapshot {
    std::string type;            // entity type id (project namespace)
    Position position;
    Health health;
    float tickInterval{ 0.0f };  // 0 = every tick; > 0 = sleeping at interval
    std::vector<ComponentData> components;
};

class IEntityWorld {
public:
    virtual ~IEntityWorld() = default;

    // ---- Lifecycle (spawn/despawn/pooling) --------------------------------
    virtual EntityId spawn(const std::string& type, const Position& position,
                           std::string& errorOut) = 0;
    virtual bool despawn(EntityId handle) = 0;
    virtual bool alive(EntityId handle) const = 0;

    // ---- Builtin components ------------------------------------------------
    virtual bool set_health(EntityId handle, const Health& health) = 0;
    virtual bool get_health(EntityId handle, Health& out) const = 0;
    virtual bool set_position(EntityId handle, const Position& position) = 0;
    virtual bool get_position(EntityId handle, Position& out) const = 0;
    virtual std::string type_of(EntityId handle) const = 0;

    // Sleeping: entities with interval > 0 tick at most once per interval.
    virtual bool set_tick_interval(EntityId handle, float interval) = 0;
    // Reads the tick interval (0 = every tick). False for a dead handle.
    virtual bool get_tick_interval(EntityId handle, float& out) const = 0;

    // ---- Project components (versioned opaque blobs) ----------------------
    virtual bool set_component(EntityId handle, const ComponentData& component) = 0;
    virtual bool get_component(EntityId handle, const std::string& type,
                               ComponentData& out) const = 0;
    // Enumerates every component of an entity in deterministic order (sorted
    // by component type). Used to migrate an entity between worlds (transfer)
    // without losing any component. The visit may not mutate the world.
    virtual void for_each_component(
        EntityId handle,
        const std::function<void(const ComponentData&)>& visit) const = 0;

    // ---- Headless simulation -----------------------------------------------
    // Advances the world. `onTick` runs for every entity whose tick policy
    // allows it this step, with the effective dt.
    virtual void tick(float dt,
                      const std::function<void(EntityId, float)>& onTick) = 0;
    virtual std::size_t size() const = 0;
    virtual std::size_t sleeping_count() const = 0;

    // ---- Spatial queries (voxel chunk grid, 16x16 columns) -----------------
    virtual std::vector<EntityId> entities_in_chunk(int cx, int cz) const = 0;
    virtual std::vector<EntityId> entities_in_aabb(float minX, float minY,
                                                   float minZ, float maxX,
                                                   float maxY, float maxZ) const = 0;

    // ---- Global enumeration (renderers, debug overlays, global systems) -----
    // Visits every live entity in deterministic registry order. The visit may
    // not spawn/despawn (the iteration is a snapshot of the current
    // population); collect and act after the loop instead.
    virtual void for_each_entity(
        const std::function<void(EntityId)>& visit) const = 0;

    // ---- Persistence (versioned, ready for the world save / replication) ---
    virtual std::vector<EntitySnapshot> serialize_entities() const = 0;
    // All-or-nothing: validates every snapshot before mutating; on failure the
    // current population is untouched.
    virtual bool deserialize_entities(const std::vector<EntitySnapshot>& entities,
                                      std::string& errorOut) = 0;
};

// EnTT-backed implementation (the only TU that includes EnTT headers).
std::unique_ptr<IEntityWorld> create_entity_world();

}  // namespace entity
}  // namespace engine
