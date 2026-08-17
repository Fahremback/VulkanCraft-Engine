#pragma once

// Public hierarchical local spaces contract (SDK, META §19 / FALTANTES §15
// item "espaços locais hierárquicos para planetas e veículos grandes"). A
// world may contain a TREE of local frames ("spaces") — e.g. a planet, a
// vehicle docked on it, a seat inside the vehicle — each carrying a rigid
// transform (position + yaw) relative to its parent. Entities can be BOUND to
// a space: their stored position becomes LOCAL to that space's frame, so the
// frame's transform absorbs the large absolute offsets (double) while the
// entity's float position stays small and exact.
//
// A space that MOVES (a vehicle driving, a planet rotating) carries all
// entities bound to it AND to its descendants for free: the binding is
// space-local, so the engine only updates the space transform.
//
// The binding is stored as a RESERVED project component (`engine.space_ref`)
// ON the entity — it travels with the world save and the replication region
// automatically, exactly like `engine.world_ref` (no separate registry to
// keep in sync). Spaces themselves are runtime state owned by the project
// (recreated on boot); the per-entity binding is what persists.
//
// This header is self-contained; the only implementation lives in
// src/engine/sdk/LocalSpace.cpp.

#include "engine/entity/IEntityWorld.hpp"
#include "engine/world/IWorldManager.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace engine {
namespace world {

// Reserved component type that marks an entity as bound to a local space.
// Blob: JSON `{"space":"<name>"}`. Entities WITHOUT this component live in the
// world frame directly (their stored position is world-absolute).
constexpr const char* kSpaceRefComponent = "engine.space_ref";

// Rigid transform of a space relative to its parent frame: position (double,
// parent-frame coordinates) + rotation around Y (`yawDegrees`).
struct SpaceTransform {
    glm::dvec3 position{ 0.0, 0.0, 0.0 };
    float yawDegrees{ 0.0f };
};

class ILocalSpace {
public:
    virtual ~ILocalSpace() = default;

    // ---- Space hierarchy ----------------------------------------------------
    // Registers a space named `name` attached to `parentName` ("" = the world
    // frame itself, the implicit root). Fails all-or-nothing: empty/duplicate
    // name, unknown parent. The space starts with the given transform.
    virtual bool create_space(const std::string& name,
                              const std::string& parentName,
                              const SpaceTransform& transform,
                              std::string& errorOut) = 0;
    // Removes a space; refused when it still has child spaces (the tree must
    // stay connected) or when any entity is bound to it (unbind first).
    virtual bool remove_space(const std::string& name,
                              std::string& errorOut) = 0;

    virtual bool space_exists(const std::string& name) const = 0;
    // Parent space name ("" = world frame root).
    virtual std::string space_parent(const std::string& name) const = 0;
    // Reads the current transform; false for an unknown space.
    virtual bool space_transform(const std::string& name,
                                 SpaceTransform& out) const = 0;
    // Replaces the space's transform relative to its parent. All entities
    // bound to the space or its descendants keep their LOCAL positions and
    // follow the frame (a moving vehicle carries its occupants).
    virtual bool set_space_transform(const std::string& name,
                                     const SpaceTransform& transform) = 0;
    // Shifts the space's position by `delta` (parent-frame units). Same
    // carrying semantics as set_space_transform.
    virtual bool move_space(const std::string& name,
                            const glm::dvec3& delta) = 0;

    // ---- Frame conversions (lossless in double) -----------------------------
    // Converts a position in the space's local frame to ABSOLUTE world
    // coordinates (climbing the parent chain). False for an unknown space.
    virtual bool space_to_world(const std::string& name,
                                const glm::dvec3& local,
                                glm::dvec3& worldOut,
                                std::string& errorOut) const = 0;
    // Inverse: absolute world coordinates -> local frame of the space.
    virtual bool world_to_space(const std::string& name,
                                const glm::dvec3& world,
                                glm::dvec3& localOut,
                                std::string& errorOut) const = 0;

    // ---- Entity binding -----------------------------------------------------
    // Binds a live entity to `spaceName` at `localPos` (space-local float
    // coordinates — small because the space transform absorbs the big
    // offsets). The entity's stored position becomes `localPos` and the
    // reserved `engine.space_ref` component records the space. Fails
    // all-or-nothing: unknown world/space, dead entity, entity already bound.
    virtual bool bind_entity(const std::string& worldName,
                             engine::entity::EntityId handle,
                             const std::string& spaceName,
                             const glm::vec3& localPos,
                             std::string& errorOut) = 0;
    // The space the entity is bound to ("" = unbound / world frame). Reads
    // the reserved component, so it survives save/load without a registry.
    virtual std::string entity_space(const std::string& worldName,
                                     engine::entity::EntityId handle) const = 0;
    // Unbinds the entity back to the world frame, converting its position to
    // the absolute world frame (so it does not teleport). The reserved
    // component is removed. False for a dead handle; a missing binding is a
    // no-op that returns true.
    virtual bool unbind_entity(const std::string& worldName,
                               engine::entity::EntityId handle) = 0;

    // Absolute world position of an entity: bound -> space chain + local;
    // unbound -> stored position (world frame). False for unknown world/dead
    // handle.
    virtual bool entity_world_position(const std::string& worldName,
                                       engine::entity::EntityId handle,
                                       glm::dvec3& out,
                                       std::string& errorOut) const = 0;
};

// The only implementation of ILocalSpace (src/engine/sdk/LocalSpace.cpp).
std::unique_ptr<ILocalSpace> create_local_space(IWorldManager& manager);

}  // namespace world
}  // namespace engine
