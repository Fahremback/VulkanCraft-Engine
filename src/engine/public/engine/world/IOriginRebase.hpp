#pragma once

// Public origin-rebasing contract (SDK, META §19 / FALTANTES §15 item
// "origin rebasing ou coordenadas de alta precisão"). The engine's source of
// truth for positions is the ABSOLUTE world frame, but float32 consumers
// (entity positions, physics, rendering) lose resolution far from the origin:
// at |x| ~ 1e9 a float32 cannot even distinguish adjacent blocks (ulp = 64).
//
// This service keeps the ABSOLUTE coordinates in double precision (glm::dvec3)
// and maintains a LOCAL frame origin that follows the gameplay focus: entity
// positions are stored relative to the origin, so they stay small and the
// stored float32 is exact at sub-block resolution. A REBASE shifts the origin
// and translates every entity's stored position by the delta, leaving absolute
// positions preserved for content near the new origin — float32 is exact only
// near the current origin, and the rebase is designed to follow the focus, so
// active content always sits exactly there (see rebase() for the precise
// guarantee).
//
// All worlds of the bound IWorldManager share ONE absolute frame (portals
// already map positions between worlds in that frame); the service translates
// the entities of every world on each rebase. COMPOSITION with hierarchical
// local spaces (ILocalSpace): an entity bound to a space stores its position
// relative to that space's frame, so a rebase SKIPS it — the space transform
// (absolute, double) absorbs the world offset and is untouched. This header
// is self-contained; the only implementation lives in
// src/engine/sdk/OriginRebase.cpp.

#include "engine/entity/IEntityWorld.hpp"
#include "engine/world/IWorldManager.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace world {

// Absolute world coordinate (double precision) — the source of truth. The
// local frame origin is always recoverable, so any absolute position can be
// represented exactly as origin + small local offset.
using WorldCoord = glm::dvec3;

// Result of a rebase / focus update.
struct RebaseResult {
    bool rebased{ false };                // true when the origin actually moved
    glm::dvec3 delta{ 0.0, 0.0, 0.0 };    // origin shift applied (new - old)
    std::size_t translatedEntities{ 0 };  // entities shifted by -delta
};

class IOriginRebase {
public:
    virtual ~IOriginRebase() = default;

    // Current origin of the local frame (absolute, double precision).
    virtual glm::dvec3 origin() const = 0;

    // ---- Lossless conversions ----------------------------------------------
    // Absolute -> local (float): absolute - origin, computed in double and
    // cast. The composition to_absolute_d(to_local(a)) == a is exact whenever
    // |a - origin| is float-representable (the whole point of rebasing).
    virtual glm::vec3 to_local(const glm::dvec3& absolute) const = 0;
    // Local (float) -> absolute (double): origin + local.
    virtual glm::dvec3 to_absolute_d(const glm::vec3& local) const = 0;

    // ---- Rebase (shift the local frame, keep absolute positions) ------------
    // Moves the origin to `newOrigin` and translates every entity in EVERY
    // world by -(newOrigin - oldOrigin). Absolute positions are preserved
    // BIT-EXACT for entities near the new origin (|local| small enough to be
    // float-exact) — the intended use is rebasing to the focus, so active
    // content always meets this. For entities far from BOTH origins the
    // translated float position degrades exactly as any float32 at that
    // magnitude would (documented limitation, not a data loss: content far
    // from the focus is streamed/culled anyway). Transactional all-or-nothing:
    // entities are collected first, then applied; on any failure every
    // already-applied entity is restored to its old local position and the
    // origin is NOT committed. Rebase by zero delta is a no-op success.
    virtual RebaseResult rebase(const glm::dvec3& newOrigin,
                                std::string& errorOut) = 0;

    // ---- Gameplay loop (focus-driven auto rebase) ---------------------------
    // If |focus - origin| exceeds `threshold`, rebase the origin to the focus
    // — snapped to the integer voxel grid when `snapToVoxel` is true, which
    // keeps the block mapping floor(absolute) integer-exact. Otherwise a
    // no-op (rebased = false, nothing translated). Call once per frame with
    // the absolute player/camera position.
    virtual RebaseResult update(const glm::dvec3& focus, double threshold,
                                bool snapToVoxel, std::string& errorOut) = 0;

    // ---- High-precision convenience ----------------------------------------
    // Spawns an entity at an ABSOLUTE position (converted to the local frame
    // internally). Refused all-or-nothing for an unknown world.
    virtual engine::entity::EntityId spawn_at(const std::string& worldName,
                                              const std::string& type,
                                              const glm::dvec3& absolute,
                                              std::string& errorOut) = 0;
    // Reads an entity's ABSOLUTE position (origin + stored local). Fails with
    // a diagnostic for an unknown world or a dead handle.
    virtual bool absolute_position(const std::string& worldName,
                                   engine::entity::EntityId handle,
                                   glm::dvec3& out,
                                   std::string& errorOut) const = 0;
};

// The only implementation of IOriginRebase (src/engine/sdk/OriginRebase.cpp).
// Binds to an existing manager; the service does not own it.
std::unique_ptr<IOriginRebase> create_origin_rebase(IWorldManager& manager);

}  // namespace world
}  // namespace engine
