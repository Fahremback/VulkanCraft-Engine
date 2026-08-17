#pragma once

// DebrisRuntime (FALTANTES §16 item 11): production debris management for the
// destruction pipeline (items 8–10). Debris = dynamic bodies detached from
// terrain/fracture by explosions. The manager owns their full lifecycle:
//
//   pooling   — debris bodies are REUSED, not recreated: on despawn a body is
//               parked (kinematic, moved to a far corner) and returned to the
//               pool; a later spawn reuses it (set_motion + set_transform +
//               impulse) instead of allocating a new Jolt body. Observability:
//               reused_count vs spawned_total prove the pool.
//   LOD       — update(focus) culls debris beyond cullRadius (destroyed from
//               the simulation, record kept with the last transform); when
//               focus returns, the debris re-enters from the pool at the same
//               transform. Far debris stop simulating; near debris don't.
//   persist   — snapshot_persistable() captures ONLY resting debris (sleeping
//               or below restSpeed) within persistRadius whose mass is >=
//               minPersistMass (dust/flying/far debris are dropped — selective
//               persistence); restore() rebuilds them from the pool.
//   replicate — every lifecycle change (spawn / despawn / settle) becomes a
//               DebrisReplicationEvent drained once by the network layer.
//
// Self-contained: depends only on the physics runtime + the PUBLIC voxel
// world contract (used by revoxelize_sleeping). Deterministic: ordered
// snapshots/events, fixed pool order.

#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Engine::Gameplay {

struct DebrisConfig {
    std::size_t poolSize{ 32 };      // max parked bodies kept for reuse
    float cullRadius{ 96.0f };       // beyond focus -> culled (out of sim)
    float persistRadius{ 64.0f };    // resting debris within -> persistable
    float restSpeed{ 0.05f };        // speed below which debris counts as resting
    float minPersistMass{ 0.5f };    // lighter debris never persists (dust)
    glm::vec3 poolCorner{ 0.0f, -4096.0f, 0.0f };  // parking spot (far from play)

    // Data-driven: JSON keys poolSize / cullRadius / persistRadius /
    // restSpeed / minPersistMass. Out-of-range values are REFUSED with a
    // diagnostic (never clamped), mirroring FractureConfig/ExplosionConfig.
    bool load_from_json(const std::string& json, std::string& errorOut);
};

// One persistable debris record (selective persistence): identity, transform,
// shape, motion and material. Ordered deterministically by id for round-trips.
struct DebrisPersistRecord {
    std::uint64_t id{ 0 };
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    Physics::ColliderShape shape{ Physics::BoxShape{} };
    float mass{ 1.0f };
    std::uint32_t materialIndex{ 0 };
    bool sleeping{ false };
};

// Revoxelization policy (FALTANTES §16 item 12): the ASSET policy that decides
// which sleeping debris are written back into the voxel world as solid blocks
// (the destruction converges: debris becomes terrain again) and which are
// skipped/kept. Data-driven via JSON.
struct RevoxelizePolicy {
    bool enabled{ true };
    float settleDelay{ 1.0f };          // resting seconds before eligible
    std::uint32_t materialFilter{ 0 };  // 0 = any; else only this materialIndex
    int maxBlocksPerDebris{ 32 };       // voxel footprint cap per debris
    int maxDebrisPerPass{ 16 };         // budget per revoxelize_sleeping call
    bool removeAfter{ true };           // despawn the debris after writing

    // JSON keys: enabled / settleDelay / materialFilter / maxBlocksPerDebris /
    // maxDebrisPerPass / removeAfter. Out-of-range values are REFUSED with a
    // diagnostic (never clamped), mirroring the other gameplay configs.
    bool load_from_json(const std::string& json, std::string& errorOut);
};

struct RevoxelizeResult {
    std::size_t debrisRevoxelized{ 0 };  // debris written back + despawned
    std::size_t blocksWritten{ 0 };      // air cells turned solid
    std::size_t debrisSkipped{ 0 };      // eligible but no block mapping
};

// Lifecycle event for the replication layer. Settled fires once when a debris
// transitions to resting (persisted afterwards); Spawned/Despawned carry the
// transform so clients can spawn/move/remove their visual.
struct DebrisReplicationEvent {
    enum class Type : std::uint8_t { Spawned, Despawned, Settled };
    Type type{ Type::Spawned };
    std::uint64_t id{ 0 };
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 linearVelocity{ 0.0f };
    float mass{ 1.0f };
    std::uint32_t materialIndex{ 0 };
};

class DebrisRuntime final {
public:
    // Does not take ownership of physics; it must outlive the runtime.
    explicit DebrisRuntime(Physics::PhysicsRuntime& physics,
                           DebrisConfig config = {});

    // Pooling spawn: acquires a debris body (reuses a parked one when
    // available). `materialIndex` tags the debris for persistence/replication.
    Physics::BodyHandle spawn(const Physics::BodyDesc& description,
                              std::uint32_t materialIndex);

    // Pooling despawn: parks the body (kinematic + pool corner) and returns
    // it to the pool instead of destroying it. Emits a Despawned event.
    bool despawn(Physics::BodyHandle body);

    // LOD tick: culls debris beyond cullRadius from `focus` (kept as a record,
    // re-entered from the pool when focus returns); marks resting debris.
    void update(const glm::vec3& focus, float deltaTime);

    // Selective persistence: resting debris within persistRadius of `focus`
    // with mass >= minPersistMass, deterministic order (by id).
    std::vector<DebrisPersistRecord> snapshot_persistable(
        const glm::vec3& focus) const;

    // History snapshots (FALTANTES §16 item 13): every ACTIVE debris whose
    // position lies inside the given world AABB, in any motion state (flying,
    // falling, resting — persistence filters do NOT apply). Deterministic
    // order (active_ order). Used by DestructionHistoryRuntime::capture.
    std::vector<DebrisPersistRecord> debris_in_box(
        const glm::vec3& minimum, const glm::vec3& maximum) const;

    // Despawns every ACTIVE debris whose position lies inside the given world
    // AABB (pooling: bodies are parked, not destroyed). Returns how many were
    // despawned. Deterministic (active_ order). Used by
    // DestructionHistoryRuntime::restore to clear the region before
    // re-spawning the captured records.
    std::size_t despawn_in_box(const glm::vec3& minimum,
                               const glm::vec3& maximum);

    // Restores a snapshot (spawns through the pool). Returns how many
    // records were re-created.
    std::size_t restore(const std::vector<DebrisPersistRecord>& records);

    // Replication: drains the lifecycle events since the last call.
    std::vector<DebrisReplicationEvent> drain_replication_events();

    // Revoxelization (FALTANTES §16 item 12): sleeping debris that met the
    // policy's settle delay are written back into the voxel world (air cells
    // inside the debris footprint become `blockOf(materialIndex)` solid
    // blocks, capped per debris) and despawned. `blockOf` maps a debris
    // materialIndex to a world block id; a mapping to air (0) skips the
    // debris (no terrain for that material). Deterministic order (active_
    // order, fixed cell scan).
    RevoxelizeResult revoxelize_sleeping(
        engine::voxel::IVoxelWorld& world, const RevoxelizePolicy& policy,
        const std::function<std::uint32_t(std::uint32_t)>& blockOf);

    // Observability (gates/tests/telemetry).
    std::size_t active_count() const noexcept { return active_.size(); }
    std::size_t culled_count() const noexcept { return culled_.size(); }
    std::size_t pooled_count() const noexcept { return parked_.size(); }
    std::size_t spawned_total() const noexcept { return spawnedTotal_; }
    std::size_t reused_count() const noexcept { return reusedCount_; }
    std::size_t cull_enter_count() const noexcept { return cullEnter_; }
    std::size_t cull_reenter_count() const noexcept { return cullReenter_; }
    const DebrisConfig& config() const noexcept { return config_; }

private:
    struct Debris {
        Physics::BodyHandle body{ Physics::InvalidBody };
        std::uint64_t id{ 0 };
        glm::vec3 position{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        Physics::ColliderShape shape{ Physics::BoxShape{} };
        float mass{ 1.0f };
        std::uint32_t materialIndex{ 0 };
        float restTimer{ 0.0f };
        bool resting{ false };
        bool settled{ false };
    };
    Debris* find(Physics::BodyHandle body);
    const Debris* find(Physics::BodyHandle body) const;
    void park(Physics::BodyHandle body);
    void emit(Physics::BodyHandle body, DebrisReplicationEvent::Type type);
    std::uint64_t nextId_{ 1 };

    Physics::PhysicsRuntime& physics_;
    DebrisConfig config_;
    std::vector<Physics::BodyHandle> parked_;   // pool (parked bodies)
    std::vector<Debris> active_;                // live debris
    std::vector<Debris> culled_;                // LOD-out records (transform kept)
    std::vector<DebrisReplicationEvent> events_;
    std::size_t spawnedTotal_{ 0 };
    std::size_t reusedCount_{ 0 };
    std::size_t cullEnter_{ 0 };
    std::size_t cullReenter_{ 0 };
};

}  // namespace Engine::Gameplay
