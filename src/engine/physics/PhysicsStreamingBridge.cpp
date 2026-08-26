#include "engine/physics/PhysicsStreamingBridge.hpp"

#include <cmath>
#include <iterator>
#include <set>

namespace Engine::Physics {

namespace {

bool aabb_contains(const glm::vec3& minimum, const glm::vec3& maximum,
                   const glm::vec3& point) noexcept {
    return point.x >= minimum.x && point.x <= maximum.x &&
           point.y >= minimum.y && point.y <= maximum.y &&
           point.z >= minimum.z && point.z <= maximum.z;
}

}  // namespace

PhysicsStreamingBridge::ChunkKey
PhysicsStreamingBridge::chunk_of(const glm::vec3& position,
                                 float chunkSize) noexcept {
    const int cx = static_cast<int>(std::floor(position.x / chunkSize));
    const int cz = static_cast<int>(std::floor(position.z / chunkSize));
    return { cx, cz };
}

PhysicsStreamingBridge::PhysicsStreamingBridge(
    engine::voxel::IVoxelWorld& world, PhysicsRuntime& physics, Config config)
    : world_(world), physics_(physics), config_(config) {}

void PhysicsStreamingBridge::refresh_materials() {
    materials_.clear();
    for (const engine::voxel::BlockRuntimeView& view : world_.runtime_block_views()) {
        Material material;
        material.friction = view.friction;
        material.restitution = view.bounciness;
        material.density = view.density;
        materials_.emplace(view.id, material);
    }
}

void PhysicsStreamingBridge::create_terrain(int cx, int cz) {
    const float size = config_.chunkWorldSize;
    const float centerX = (static_cast<float>(cx) + 0.5f) * size;
    const float centerZ = (static_cast<float>(cz) + 0.5f) * size;

    // Probe the world surface at the chunk center: the terrain slab's top sits
    // exactly at the first solid block's top face. A miss (floating island /
    // air column) falls back to a floor at y=0 so dynamic bodies never tunnel
    // out of the world.
    const engine::voxel::VoxelRaycastHit down = world_.raycast(
        glm::vec3(centerX, config_.raycastTop, centerZ),
        glm::vec3(0.0f, -1.0f, 0.0f),
        config_.raycastTop + config_.terrainThickness);
    const float topY = down.hit ? down.position.y : 0.0f;

    BodyDesc slab;
    slab.motion = MotionType::Static;
    slab.position = glm::vec3(centerX, topY - config_.terrainThickness * 0.5f,
                              centerZ);
    slab.collider.shape = BoxShape{ glm::vec3(size * 0.5f,
                                              config_.terrainThickness * 0.5f,
                                              size * 0.5f) };
    // Physical material (FALTANTES §16 item 7): the slab carries the surface
    // block's friction/restitution (ice slides, rubber bounces). The table
    // comes from the world's public runtime views — the registry JSON is the
    // single source.
    if (materials_.empty()) refresh_materials();
    if (down.hit) {
        const std::uint32_t surfaceBlock = world_.get_block(
            down.block.x, down.block.y, down.block.z);
        const auto found = materials_.find(surfaceBlock);
        if (found != materials_.end()) {
            slab.collider.friction = found->second.friction;
            slab.collider.restitution = found->second.restitution;
        }
    }
    const BodyHandle body = physics_.create_body(slab);
    if (body == InvalidBody) return;
    terrain_.emplace(ChunkKey{ cx, cz }, body);
    ++spawnedTerrain_;
}

void PhysicsStreamingBridge::unload_chunk(int cx, int cz) {
    const ChunkKey key{ cx, cz };
    const auto it = terrain_.find(key);
    if (it != terrain_.end()) {
        physics_.destroy_body(it->second);
        terrain_.erase(it);
    }
    dynamicByChunk_.erase(key);
}

void PhysicsStreamingBridge::recompute_membership() noexcept {
    std::map<ChunkKey, std::vector<BodyHandle>> next;
    for (const auto& [handle, captured] : dynamic_) {
        const RigidBody* rb = physics_.body(handle);
        const glm::vec3 position =
            (rb != nullptr) ? rb->position : captured;
        next[chunk_of(position, config_.chunkWorldSize)].push_back(handle);
    }
    dynamicByChunk_ = std::move(next);
}

void PhysicsStreamingBridge::despawn_body(BodyHandle body) noexcept {
    dynamic_.erase(body);
    physics_.destroy_body(body);
}

void PhysicsStreamingBridge::sync(const glm::vec3& focus) {
    const float size = config_.chunkWorldSize;

    // Scan ring around the focus PLUS every tracked chunk, so a focus move far
    // away evicts correctly without scanning the whole world.
    std::set<ChunkKey> scanned;
    const int ringMinX = static_cast<int>(
        std::floor((focus.x - config_.scanRadius) / size));
    const int ringMaxX = static_cast<int>(
        std::floor((focus.x + config_.scanRadius) / size));
    const int ringMinZ = static_cast<int>(
        std::floor((focus.z - config_.scanRadius) / size));
    const int ringMaxZ = static_cast<int>(
        std::floor((focus.z + config_.scanRadius) / size));
    for (int cx = ringMinX; cx <= ringMaxX; ++cx) {
        for (int cz = ringMinZ; cz <= ringMaxZ; ++cz) {
            scanned.emplace(cx, cz);
        }
    }
    for (const auto& [key, ignored] : terrain_) { scanned.insert(key); }
    for (const auto& [key, ignored] : dynamicByChunk_) { scanned.insert(key); }

    // 1. Terrain reconciliation: loaded chunk without a slab -> create;
    //    tracked chunk that left the loaded set -> destroy its slab.
    for (const ChunkKey& key : scanned) {
        const bool loaded = world_.is_chunk_loaded(key.first, key.second);
        const auto terrainIt = terrain_.find(key);
        if (loaded && terrainIt == terrain_.end()) {
            create_terrain(key.first, key.second);
        } else if (!loaded && terrainIt != terrain_.end()) {
            unload_chunk(key.first, key.second);
        }
    }

    // 1b. Stale terrain refresh (task B.5): chunks whose surface changed
    //     since their slab was created (a transactional edit raised or
    //     lowered the surface) get their slab re-probed and rebuilt.
    for (const ChunkKey& key : staleTerrain_) {
        const bool loaded = world_.is_chunk_loaded(key.first, key.second);
        if (loaded && terrain_.find(key) != terrain_.end()) {
            unload_chunk(key.first, key.second);
            create_terrain(key.first, key.second);
        }
    }
    staleTerrain_.clear();

    // 2. Dynamic membership: reassign buckets from live body positions so a
    //    body that fell across a chunk boundary follows its chunk.
    recompute_membership();

    // 3. Authority: despawn every dynamic body whose chunk is not loaded (the
    //    streaming world no longer simulates that region). No orphan bodies.
    std::vector<ChunkKey> unloadedKeys;
    for (const auto& [key, bodies] : dynamicByChunk_) {
        if (!world_.is_chunk_loaded(key.first, key.second)) {
            unloadedKeys.push_back(key);
        }
    }
    for (const ChunkKey& key : unloadedKeys) {
        const auto it = dynamicByChunk_.find(key);
        if (it == dynamicByChunk_.end()) continue;
        std::vector<BodyHandle> bodies = std::move(it->second);
        dynamicByChunk_.erase(it);
        for (BodyHandle body : bodies) {
            if (dynamic_.erase(body) != 0) {
                physics_.destroy_body(body);
                ++unloadedBodies_;
            }
        }
    }
}

BodyHandle PhysicsStreamingBridge::spawn_dynamic(const BodyDesc& description) {
    const BodyHandle body = physics_.create_body(description);
    if (body == InvalidBody) return InvalidBody;
    dynamic_.emplace(body, description.position);
    dynamicByChunk_[chunk_of(description.position, config_.chunkWorldSize)]
        .push_back(body);
    return body;
}

bool PhysicsStreamingBridge::despawn(BodyHandle body) {
    if (dynamic_.find(body) == dynamic_.end()) return false;
    despawn_body(body);
    recompute_membership();  // rebuilds buckets from the remaining bodies
    return true;
}

const RigidBody* PhysicsStreamingBridge::body(BodyHandle handle) const {
    return physics_.body(handle);
}

std::size_t PhysicsStreamingBridge::wake_region(const glm::vec3& minimum,
                                                const glm::vec3& maximum) {
    std::size_t woken = 0;
    for (const auto& [handle, captured] : dynamic_) {
        const RigidBody* rb = physics_.body(handle);
        const glm::vec3 position =
            (rb != nullptr) ? rb->position : captured;
        if (aabb_contains(minimum, maximum, position)) {
            physics_.wake(handle);
            ++woken;
        }
    }
    return woken;
}

std::size_t PhysicsStreamingBridge::note_region_edited(
    const glm::vec3& minimum, const glm::vec3& maximum) {
    // 1. Wake bodies in the region (same semantics as wake_region).
    const std::size_t woken = wake_region(minimum, maximum);

    // 2. Mark every tracked terrain chunk overlapping the AABB as stale.
    //    The next sync() will re-probe its surface and rebuild the slab.
    const float size = config_.chunkWorldSize;
    const int minCX = static_cast<int>(std::floor(minimum.x / size));
    const int maxCX = static_cast<int>(std::floor(maximum.x / size));
    const int minCZ = static_cast<int>(std::floor(minimum.z / size));
    const int maxCZ = static_cast<int>(std::floor(maximum.z / size));
    for (int cx = minCX; cx <= maxCX; ++cx) {
        for (int cz = minCZ; cz <= maxCZ; ++cz) {
            // Only stale slabs that actually exist — a chunk that is not yet
            // loaded has no slab, and sync() will create one when it loads.
            if (terrain_.find({ cx, cz }) != terrain_.end()) {
                staleTerrain_.emplace(cx, cz);
            }
        }
    }
    return woken;
}

BodyHandle PhysicsStreamingBridge::island_body_at(std::size_t index) const noexcept {
    if (index >= islandBodies_.size()) return InvalidBody;
    auto it = islandBodies_.begin();
    std::advance(it, index);
    return it->second;
}

std::size_t PhysicsStreamingBridge::sync_detached_islands(
    const VoxelConnectivity& connectivity,
    const ConnectivitySettings& settings,
    const std::function<bool(std::uint32_t)>& isSolid) {
    const std::vector<VoxelIsland> islands =
        connectivity.sync(world_, settings, isSolid);
    if (materials_.empty()) refresh_materials();
    std::size_t spawned = 0;
    for (const VoxelIsland& island : islands) {
        const auto key = std::make_tuple(island.minimum.x,
                                         island.minimum.y,
                                         island.minimum.z);
        if (islandBodies_.find(key) != islandBodies_.end()) continue;

        const glm::ivec3 size =
            island.maximum - island.minimum + glm::ivec3(1);
        const glm::vec3 center =
            glm::vec3(island.minimum) + glm::vec3(size) * 0.5f;

        // Mass = solid cell count * the density of the block at the island's
        // min corner (the material table mirrors the registry JSON — the
        // single source).
        float density = 1.0f;
        const auto material = materials_.find(world_.get_block(
            island.minimum.x, island.minimum.y, island.minimum.z));
        if (material != materials_.end()) density = material->second.density;

        BodyDesc desc;
        desc.motion = MotionType::Dynamic;
        desc.position = center;
        desc.mass = static_cast<float>(island.solidCells) * density;
        desc.collider.shape = BoxShape{ glm::vec3(size) * 0.5f };
        const BodyHandle body = spawn_dynamic(desc);
        if (body == InvalidBody) continue;
        islandBodies_.emplace(key, body);
        ++spawned;
    }
    return spawned;
}

}  // namespace Engine::Physics
