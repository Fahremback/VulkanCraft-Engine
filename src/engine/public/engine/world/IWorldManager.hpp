#pragma once

// Public world manager contract (SDK, META section 15 / FALTANTES item 8 —
// multiple worlds, dimensions and portals). A manager owns N independent
// worlds, each with its own seed, clock, rules and persistence, plus generic
// portals that map positions between world spaces. Entity transfer between
// worlds is transactional: on any failure the source world is untouched.
//
// This header is self-contained (no renderer details, no Vulkan); the only
// implementation lives in src/engine/sdk/WorldManager.cpp.

#include "engine/entity/IEntityWorld.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace world {

// Per-world configuration. `name` must be unique; `seed` is the per-world
// seed (the project's generator/behavior RNG); `rulesJson` is an optional
// opaque JSON document whose semantics belong to the project (validated as
// well-formed JSON at create time, never guessed); `savePath` is the default
// persistence location for load_world/save_world.
struct WorldSpec {
    std::string name;
    uint64_t seed{ 0 };
    std::string rulesJson;
    std::string savePath;
};

// Read-only per-world metadata.
struct WorldInfo {
    std::string name;
    uint64_t seed{ 0 };
    std::string rulesJson;
    std::string savePath;
    bool loaded{ false };
    double elapsedSeconds{ 0.0 };  // per-world clock, advanced by update_world
    std::size_t entityCount{ 0 };
};

// A PERSISTENT reference from one entity (in one world) to an entity in
// another world (META §19 "referências persistentes entre mundos"). The
// target is named by (world name + STABLE id), never by the local generational
// handle — a handle is world-local and recycled after a despawn, while the
// stable id survives save/load and transfer, so the reference keeps resolving
// after reloads, restarts and entity migrations.
struct WorldEntityRef {
    std::string toWorld;    // destination world name
    std::string stableId;   // the target's stable id (IEntityWorld::set_stable_id)

    bool valid() const { return !toWorld.empty() && !stableId.empty(); }
};

// A portal is a generic mapping between two world spaces: an entity crossing
// it keeps its local offset from the source anchor, rotated by `yawDegrees`
// around Y, and lands at the destination anchor + rotated offset.
struct PortalSpec {
    std::string fromWorld;
    float fromX{ 0.0f };
    float fromY{ 0.0f };
    float fromZ{ 0.0f };
    std::string toWorld;
    float toX{ 0.0f };
    float toY{ 0.0f };
    float toZ{ 0.0f };
    float yawDegrees{ 0.0f };
};

class IWorldManager {
public:
    virtual ~IWorldManager() = default;

    // ---- World lifecycle (create/load/unload independently) ---------------
    // Registers and constructs a fresh world. Fails all-or-nothing when the
    // name is taken or the rules JSON is malformed.
    virtual bool create_world(const WorldSpec& spec, std::string& errorOut) = 0;
    // Registers + constructs + loads the world save from spec.savePath.
    // All-or-nothing: on load failure the world is NOT registered.
    virtual bool load_world(const WorldSpec& spec, std::string& errorOut) = 0;
    virtual bool unload_world(const std::string& name) = 0;

    virtual bool has_world(const std::string& name) const = 0;
    virtual std::vector<std::string> world_names() const = 0;
    virtual std::size_t world_count() const = 0;
    virtual WorldInfo world_info(const std::string& name) const = 0;

    // Live world access; nullptr when absent.
    virtual engine::voxel::IVoxelWorld* world(const std::string& name) = 0;
    virtual const engine::voxel::IVoxelWorld* world(
        const std::string& name) const = 0;

    // ---- Per-world clock / persistence -------------------------------------
    // Advances the named world's clock and runs its headless update
    // (generation/meshing/fluid). Worlds that are not updated do not advance.
    virtual bool update_world(const std::string& name,
                              const glm::vec3& playerPosition,
                              float deltaTime) = 0;
    virtual bool save_world(const std::string& name, const std::string& path,
                            std::string& errorOut) = 0;

    // ---- Transactional entity transfer (commit/rollback) -------------------
    // Moves a live entity from one world to another, rebuilding it at
    // `targetPosition` with health, tick interval and ALL components. Atomic:
    // the source entity is removed only after the destination accepts the
    // rebuild; any failure leaves both worlds untouched and returns an
    // invalid handle. Returns the new handle in the destination.
    virtual engine::entity::EntityId transfer_entity(
        const std::string& fromWorld, engine::entity::EntityId handle,
        const std::string& toWorld,
        const engine::entity::Position& targetPosition,
        std::string& errorOut) = 0;

    // ---- Portals (generic mappings between spaces) -------------------------
    virtual uint32_t create_portal(const PortalSpec& spec,
                                   std::string& errorOut) = 0;
    virtual bool remove_portal(uint32_t portalId) = 0;
    virtual bool portal(uint32_t portalId, PortalSpec& out) const = 0;
    virtual std::vector<uint32_t> portals() const = 0;
    // Transfers `handle` through `portalId`: the position is mapped through
    // the portal (source anchor -> yaw -> destination anchor) and the entity
    // is transfer_entity'd — same all-or-nothing guarantee.
    virtual engine::entity::EntityId transfer_via_portal(
        const std::string& worldName, engine::entity::EntityId handle,
        uint32_t portalId, std::string& errorOut) = 0;

    // ---- Persistent references between worlds (META §19) -------------------
    // Sets the persistent reference OF `fromEntity` (in `fromWorld`) to the
    // entity named by `ref` (world + stable id). The reference is stored as a
    // reserved project component (engine.world_ref) ON the entity itself, so
    // it travels with the world save and the replication region — no separate
    // registry to keep in sync. Fails all-or-nothing: unknown worlds, a dead
    // source entity, an invalid/empty ref or a same-world ref are refused
    // without mutating.
    virtual bool set_entity_ref(const std::string& fromWorld,
                                engine::entity::EntityId fromEntity,
                                const WorldEntityRef& ref,
                                std::string& errorOut) = 0;
    // Reads the entity's persistent reference (empty/invalid when none).
    virtual WorldEntityRef entity_ref(const std::string& fromWorld,
                                      engine::entity::EntityId fromEntity) const = 0;
    // Clears the reference (returns false for a dead entity; a missing ref is
    // a no-op that returns true).
    virtual bool clear_entity_ref(const std::string& fromWorld,
                                  engine::entity::EntityId fromEntity) = 0;
    // Resolves the entity's reference to the LIVE entity in the destination
    // world: looks up the destination world, resolves the stable id to the
    // live entity (survives despawn/transfer because the stable id is
    // persistent). Invalid handle + diagnostic when there is no ref, the
    // destination world is unknown/unloaded, the stable id is missing, or the
    // target is not alive.
    virtual engine::entity::EntityId resolve_entity_ref(
        const std::string& fromWorld, engine::entity::EntityId fromEntity,
        std::string& errorOut) = 0;
};

// The only implementation of IWorldManager (src/engine/sdk/WorldManager.cpp).
std::unique_ptr<IWorldManager> create_world_manager();

}  // namespace world
}  // namespace engine
