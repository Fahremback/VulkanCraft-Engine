#pragma once

// PhysicsStreamingBridge (FALTANTES §16 item 6): owns the physics bodies that
// mirror the streaming voxel world. The headless server / game runtime drive
// one tick per frame:
//
//   world.update(focus, dt);   // streaming (chunks load/unload around focus)
//   bridge.sync(focus);        // reconcile physics with the loaded chunk set
//   physics.step(dt);          // advance the world's bodies
//
// Reconciliation rules (server authority):
//   - A chunk that becomes loaded gets a static terrain slab whose top sits at
//     the world surface (probed with the voxel raycast), so dynamic bodies
//     rest on streamed terrain.
//   - A chunk that leaves the loaded set (budget shrink / focus move) is
//     unloaded: its terrain slab is destroyed AND every dynamic body whose
//     position is inside that chunk's world AABB is despawned — no orphan
//     bodies survive an unload. This is the authoritative server semantic:
//     physics never holds bodies in regions the streaming world no longer
//     simulates.
//   - Terrain edits (block changes, explosions) wake sleeping dynamic bodies
//     overlapping the edited region through wake_region().
//
// Self-contained: depends only on the PUBLIC voxel world contract
// (engine::voxel::IVoxelWorld) plus PhysicsRuntime. No Jolt/engine internals.

#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/VoxelConnectivity.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine::Physics {

class PhysicsStreamingBridge final {
public:
    struct Config {
        float chunkWorldSize{ 16.0f };  // world extent of one chunk (x/z)
        float terrainThickness{ 8.0f }; // static slab height below the surface
        float scanRadius{ 48.0f };      // loaded-chunk scan ring around focus
        float raycastTop{ 512.0f };     // world top for the surface probe
    };

    // Does not take ownership; both references must outlive the bridge.
    PhysicsStreamingBridge(engine::voxel::IVoxelWorld& world,
                           PhysicsRuntime& physics, Config config = {});

    // Reconciles the loaded-chunk set around `focus` (plus every tracked
    // chunk, so focus moves evict correctly) with physics bodies. Newly
    // loaded chunks get a static terrain slab; chunks that left the loaded
    // set are unloaded (slab destroyed, contained dynamic bodies despawned).
    void sync(const glm::vec3& focus);

    // Server-owned dynamic bodies. The bridge captures the spawn position for
    // chunk membership, so unloads can find and despawn them.
    BodyHandle spawn_dynamic(const BodyDesc& description);
    bool despawn(BodyHandle body);
    const RigidBody* body(BodyHandle handle) const;

    // Terrain edits: wakes every sleeping dynamic body whose captured
    // position is inside the world AABB. Returns how many were woken.
    std::size_t wake_region(const glm::vec3& minimum, const glm::vec3& maximum);

    // Voxel connectivity (FALTANTES §16 item 9): every DETACHED voxel island
    // (a solid component no longer connected to the anchored mass) becomes a
    // DYNAMIC Jolt body that falls under gravity; terrain still connected to
    // the anchor stays static. Idempotent: an island already turned into a
    // body is skipped (keyed by its min corner). Island bodies ride the same
    // chunk-membership/authority path as spawned bodies, so an unload
    // despawns them too. Returns how many new island bodies were spawned.
    std::size_t sync_detached_islands(
        const VoxelConnectivity& connectivity,
        const ConnectivitySettings& settings,
        const std::function<bool(std::uint32_t)>& isSolid);

    // Observability (gates/tests/telemetry).
    std::size_t terrain_body_count() const noexcept { return terrain_.size(); }
    std::size_t dynamic_body_count() const noexcept { return dynamic_.size(); }
    std::size_t unloaded_body_count() const noexcept { return unloadedBodies_; }
    std::size_t spawned_terrain_count() const noexcept { return spawnedTerrain_; }
    // Island body in deterministic (min-corner) order — test/telemetry aid.
    BodyHandle island_body_at(std::size_t index) const noexcept;

    std::size_t island_body_count() const noexcept { return islandBodies_.size(); }

private:
    using ChunkKey = std::pair<int, int>;

    // Physical material of one runtime block (FALTANTES §16 item 7): mirrors
    // the registry's friction/bounciness/density so terrain slabs respond
    // like the block they represent (ice slides, rubber bounces) and debris
    // mass comes from the block density (§16 item 9).
    struct Material {
        float friction{ 0.5f };
        float restitution{ 0.0f };
        float density{ 1.0f };
    };

    static ChunkKey chunk_of(const glm::vec3& position, float chunkSize) noexcept;
    // Builds the id -> material table from the world's public runtime views
    // (single source: the registry JSON). Called lazily on first use.
    void refresh_materials();
    void create_terrain(int cx, int cz);
    void unload_chunk(int cx, int cz);
    void recompute_membership() noexcept;

    // Detached-island bodies keyed by the island's min corner (idempotence).
    std::map<std::tuple<int, int, int>, BodyHandle> islandBodies_;
    void despawn_body(BodyHandle body) noexcept;

    engine::voxel::IVoxelWorld& world_;
    PhysicsRuntime& physics_;
    Config config_;

    std::unordered_map<std::uint32_t, Material> materials_;
    std::map<ChunkKey, BodyHandle> terrain_;    // chunk -> static slab body
    std::unordered_map<BodyHandle, glm::vec3> dynamic_;            // handle -> captured position
    std::map<ChunkKey, std::vector<BodyHandle>> dynamicByChunk_;   // chunk -> dynamic bodies
    std::size_t unloadedBodies_{ 0 };
    std::size_t spawnedTerrain_{ 0 };
};

}  // namespace Engine::Physics
