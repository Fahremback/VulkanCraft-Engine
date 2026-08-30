#include "WorldRenderer.hpp"

#include "World.hpp"
#include "Voxel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// One block face covers a 1x1 cell patch; the card half extent stays aligned
// with the sampled face so capture irradiance is evaluated at the real surface.
constexpr float kLumenHalfExtent = 0.5f;
// Per-chunk surface budget of the mesh->surface pass (bounded: the Lumen scene
// is a cache, not a copy of the mesh).
constexpr std::size_t kMaxLumenSurfacesPerChunk = 64;

// B.4: a ChunkId is {coord.x:int, coord.z:int, generation:uint32}. The draw-
// queue payload is a 64-bit integer, so the id is encoded faithfully in the
// supported chunk range (±32767 within the current origin — far beyond any
// streamed frontier): x|z each in 16 bits, generation in the low 32 bits.
std::uint64_t encode_chunk_id(const ChunkId& id) noexcept {
    const std::uint64_t cx = static_cast<std::uint32_t>(id.coord.x + 32768) & 0xFFFFu;
    const std::uint64_t cz = static_cast<std::uint32_t>(id.coord.z + 32768) & 0xFFFFu;
    return (cx << 48u) | (cz << 32u) | id.generation;
}

ChunkId decode_chunk_id(std::uint64_t payload) noexcept {
    ChunkId id;
    id.coord.x = static_cast<int>((payload >> 48u) & 0xFFFFu) - 32768;
    id.coord.z = static_cast<int>((payload >> 32u) & 0xFFFFu) - 32768;
    id.generation = static_cast<std::uint32_t>(payload & 0xFFFFFFFFu);
    return id;
}

}  // namespace

void WorldRenderer::configure(VkDevice device, VmaAllocator allocator) {
    device_ = device;
    allocator_ = allocator;
}

void WorldRenderer::begin_frame() {
    auto& safe = retiredBuffers_[gpuEpoch_ % FRAME_OVERLAP];
    for (const AllocatedBuffer& buffer : safe) {
        if (buffer.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
    }
    safe.clear();
    ++gpuEpoch_;
    if (drawQueues_) drawQueues_->clear();
    farTerrain_.upload_ready(device_, allocator_, &retiredBuffers_[(gpuEpoch_ - 1) % FRAME_OVERLAP]);
}

void WorldRenderer::request_far_terrain(int centerChunkX, int centerChunkZ, int reachChunks,
                                        float endpointQuality) {
    farTerrain_.request(farTerrainThreadPool_, centerChunkX, centerChunkZ, reachChunks, endpointQuality);
}

void WorldRenderer::retire_chunk(ChunkId chunk) {
    chunkRenderer_.retire(chunk, retiredBuffers_[(gpuEpoch_ - 1) % FRAME_OVERLAP]);
}

void WorldRenderer::upload_chunk(ChunkMeshResult result) {
    chunkRenderer_.upload(std::move(result), device_, allocator_, &retiredBuffers_[(gpuEpoch_ - 1) % FRAME_OVERLAP]);
    // A.3: mesh->surface pass — feed the Lumen-style surface cache from the
    // REAL chunk mesh that was just uploaded, keyed by (chunk id, revision).
    // The scene replaces the chunk's cards incrementally (no global rebuild).
    if (lumenScene_ != nullptr && result.valid) feed_lumen_scene(result);
}

void WorldRenderer::feed_lumen_scene(const ChunkMeshResult& result) {
    const auto& mesh = result.mesh;
    constexpr std::size_t kFacesPerQuad = 6;
    const std::size_t faceCount = mesh.meshVertices.size() / kFacesPerQuad;
    if (faceCount == 0) return;

    const std::uint64_t chunkId =
        (static_cast<std::uint64_t>(static_cast<std::int64_t>(result.chunk.coord.x)) << 32) |
        static_cast<std::uint32_t>(result.chunk.coord.z);

    // Dynamic-block material lookup (builtin ids resolve through the engine
    // table). The mesh vertex color is LIGHT-SCALED by the mesher, so the card
    // albedo is re-read from the world at the face centroid (unlit material
    // color) — the capture then computes bounced radiance from a true albedo.
    std::unordered_map<RuntimeBlockId, glm::vec4> dynamicAlbedo;
    std::unordered_map<RuntimeBlockId, uint8_t> dynamicEmission;
    for (const auto& [id, info] : world_.runtime_block_table()) {
        dynamicAlbedo[id] = info.color;
        dynamicEmission[id] = info.lightEmission;
    }

    const std::size_t stride = std::max<std::size_t>(
        1, (faceCount + kMaxLumenSurfacesPerChunk - 1) / kMaxLumenSurfacesPerChunk);
    std::vector<Engine::Rendering::LumenSurface> surfaces;
    surfaces.reserve(kMaxLumenSurfacesPerChunk);
    for (std::size_t face = 0; face < faceCount; face += stride) {
        const auto* v = &mesh.meshVertices[face * kFacesPerQuad];
        // Quad corners are v[0..3] then the triangulated v[0], v[2], v[3].
        const glm::vec3 centroid =
            (v[0].position + v[1].position + v[2].position + v[4].position) * 0.25f;
        const glm::vec3 normal = v[0].normal;
        const RuntimeBlockId id = world_.get_block_at(centroid);
        glm::vec4 albedo(1.0f);
        glm::vec3 emissive(0.0f);
        if (is_builtin_block(id)) {
            const BlockType type = static_cast<BlockType>(id);
            albedo = get_block_color(type, normal);
            if (is_emissive_block(type)) emissive = glm::vec3(albedo) * 1.5f;
        } else {
            const auto ait = dynamicAlbedo.find(id);
            if (ait != dynamicAlbedo.end()) albedo = ait->second;
            const auto eit = dynamicEmission.find(id);
            if (eit != dynamicEmission.end() && eit->second > 0u) {
                emissive = glm::vec3(albedo) *
                           (static_cast<float>(eit->second) / 15.0f);
            }
        }
        Engine::Rendering::LumenSurface surface;
        surface.center = centroid;
        surface.normal = normal;
        surface.halfExtent = glm::vec2(kLumenHalfExtent);
        surface.albedo = glm::vec4(albedo.r, albedo.g, albedo.b,
                                   std::max(albedo.a, 0.3f));
        surface.emissive = emissive;
        surfaces.push_back(std::move(surface));
    }
    std::string error;
    lumenScene_->replace_chunk(chunkId, result.sourceRevision, surfaces, error);
}

void WorldRenderer::cleanup(bool deviceAlreadyIdle) {
    farTerrainThreadPool_.wait_idle();
    farTerrain_.cleanup(device_, allocator_, deviceAlreadyIdle);
    chunkRenderer_.cleanup(device_, allocator_, true);
    for (auto& list : retiredBuffers_) {
        for (const AllocatedBuffer& buffer : list) {
            if (buffer.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
        }
        list.clear();
    }
}

void WorldRenderer::draw_far_surface(VkCommandBuffer commandBuffer) {
    farTerrain_.draw_near_surface(commandBuffer);
    farTerrain_.draw_far_surface(commandBuffer);
}

void WorldRenderer::collect_chunks_into_queue(const Frustum& frustum,
                                               Engine::Rendering::DrawQueue queue) {
    // B.4: the public ISceneCulling core is the visibility authority when
    // bound — its Gribb-Hartmann planes + AABB p-vertex test drive the real
    // submission culling (same math the renderer used inline, now through the
    // deterministic SDK surface). Counts feed the observable title contract.
    Engine::Rendering::Frustum sdkFrustum{};
    if (sceneCulling_ != nullptr) {
        for (int i = 0; i < 6; ++i) sdkFrustum.planes[i] = frustum.planes[i];
    }
    visibleChunkCount_ = 0;
    culledChunkCount_ = 0;
    lodSplit_ = 0;
    for (const auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        const glm::vec3 minimum(float(key.first * CHUNK_SIZE_X), 0.0f, float(key.second * CHUNK_SIZE_Z));
        const glm::vec3 maximum(float((key.first + 1) * CHUNK_SIZE_X), float(chunk->vertical_render_extent()),
                                float((key.second + 1) * CHUNK_SIZE_Z));
        const bool visible = (sceneCulling_ != nullptr)
            ? sceneCulling_->aabbVisible(sdkFrustum, minimum, maximum)
            : frustum.is_box_visible(minimum, maximum);
        if (!visible) { ++culledChunkCount_; continue; }
        ++visibleChunkCount_;
        const glm::vec3 center = (minimum + maximum) * 0.5f;
        const float depth = glm::dot(center - detailCamera_, center - detailCamera_);
        // B.4: distance LOD through the SDK core — the tier of the last
        // visible chunk this frame is the observable `lod` field of the title
        // (selectLod is stateless per chunk; selectLodHysteretic is the
        // per-entity form the app could drive with per-chunk state).
        if (sceneCulling_ != nullptr) {
            lodSplit_ = sceneCulling_->selectLod(
                std::sqrt(std::max(depth, 0.0f)));
        }
        drawQueues_->push(queue, Engine::Rendering::SceneDrawItem{
            encode_chunk_id(chunk->id()), depth, 0u });
    }
}

bool WorldRenderer::detail_chunk_visible(const Frustum& frustum, const glm::vec3& minimum,
                                         const glm::vec3& maximum, float depth) const {
    // B.4: conservative occlusion culling for the detail queues — a chunk fully
    // inside the frustum that is entirely behind a nearer opaque chunk is
    // hidden from the viewer and skipped. The SDK core proves it with the
    // projected-rect + depth test; when unset the detail pass draws as before.
    if (sceneCulling_ == nullptr) return true;
    for (const auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        const glm::vec3 occMin(float(key.first * CHUNK_SIZE_X), 0.0f, float(key.second * CHUNK_SIZE_Z));
        const glm::vec3 occMax(float((key.first + 1) * CHUNK_SIZE_X), float(chunk->vertical_render_extent()),
                               float((key.second + 1) * CHUNK_SIZE_Z));
        const glm::vec3 occCenter = (occMin + occMax) * 0.5f;
        const float occDepth = glm::dot(occCenter - detailCamera_, occCenter - detailCamera_);
        if (occDepth >= depth) continue;  // the occluder must be nearer
        if (sceneCulling_->occluded(detailViewProj_, occMin, occMax, minimum, maximum)) {
            ++occludedDetailCount_;
            return false;
        }
    }
    return true;
}

void WorldRenderer::draw_details_unlocked(VkCommandBuffer commandBuffer, const Frustum& frustum) {
    // B.4: opaque voxel chunks drawn front-to-back (early-z), ordered by the
    // previously manual pass now going through the canonical draw-queue core.
    drawQueues_->clear();
    collect_chunks_into_queue(frustum, Engine::Rendering::DrawQueue::Opaque);
    for (const auto& item : drawQueues_->sorted(Engine::Rendering::DrawQueue::Opaque)) {
        chunkRenderer_.draw(decode_chunk_id(item.payload), commandBuffer);
    }
}

void WorldRenderer::draw(VkCommandBuffer commandBuffer, const Frustum& frustum) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    farTerrain_.draw(commandBuffer);
    draw_details_unlocked(commandBuffer, frustum);
}

void WorldRenderer::draw_details(VkCommandBuffer commandBuffer, const Frustum& frustum) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    draw_details_unlocked(commandBuffer, frustum);
}

void WorldRenderer::draw_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    const int centerX = int(std::floor(center.x / float(CHUNK_SIZE_X)));
    const int centerZ = int(std::floor(center.z / float(CHUNK_SIZE_Z)));
    for (auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        if (std::abs(key.first - centerX) > chunkRadius || std::abs(key.second - centerZ) > chunkRadius) continue;
        chunkRenderer_.draw(chunk->id(), commandBuffer);
    }
}

void WorldRenderer::draw_foliage_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    const int centerX = int(std::floor(center.x / float(CHUNK_SIZE_X)));
    const int centerZ = int(std::floor(center.z / float(CHUNK_SIZE_Z)));
    for (auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        if (std::abs(key.first - centerX) > chunkRadius || std::abs(key.second - centerZ) > chunkRadius) continue;
        chunkRenderer_.draw_foliage(chunk->id(), commandBuffer);
    }
}

void WorldRenderer::draw_grass_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    const int centerX = int(std::floor(center.x / float(CHUNK_SIZE_X)));
    const int centerZ = int(std::floor(center.z / float(CHUNK_SIZE_Z)));
    for (auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        if (std::abs(key.first - centerX) > chunkRadius || std::abs(key.second - centerZ) > chunkRadius) continue;
        chunkRenderer_.draw_grass(chunk->id(), commandBuffer);
    }
}

void WorldRenderer::draw_grass(VkCommandBuffer commandBuffer, const Frustum& frustum) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    // B.4: grass is alpha-test vegetation — front-to-back via the queue core,
    // with conservative occlusion culling through the SDK core.
    drawQueues_->clear();
    occludedDetailCount_ = 0;
    collect_chunks_into_queue(frustum, Engine::Rendering::DrawQueue::Foliage);
    for (const auto& item : drawQueues_->sorted(Engine::Rendering::DrawQueue::Foliage)) {
        const ChunkId id = decode_chunk_id(item.payload);
        const auto it = world_.chunks.find({ id.coord.x, id.coord.z });
        if (it == world_.chunks.end()) continue;
        const glm::vec3 minimum(float(id.coord.x * CHUNK_SIZE_X), 0.0f, float(id.coord.z * CHUNK_SIZE_Z));
        const glm::vec3 maximum(float((id.coord.x + 1) * CHUNK_SIZE_X), float(it->second->vertical_render_extent()),
                                float((id.coord.z + 1) * CHUNK_SIZE_Z));
        if (!detail_chunk_visible(frustum, minimum, maximum, item.depth)) continue;
        chunkRenderer_.draw_grass(id, commandBuffer);
    }
}

void WorldRenderer::draw_foliage(VkCommandBuffer commandBuffer, const Frustum& frustum) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    // B.4: foliage is alpha-test vegetation — front-to-back via the queue core,
    // with conservative occlusion culling through the SDK core.
    drawQueues_->clear();
    occludedDetailCount_ = 0;
    collect_chunks_into_queue(frustum, Engine::Rendering::DrawQueue::Foliage);
    for (const auto& item : drawQueues_->sorted(Engine::Rendering::DrawQueue::Foliage)) {
        const ChunkId id = decode_chunk_id(item.payload);
        const auto it = world_.chunks.find({ id.coord.x, id.coord.z });
        if (it == world_.chunks.end()) continue;
        const glm::vec3 minimum(float(id.coord.x * CHUNK_SIZE_X), 0.0f, float(id.coord.z * CHUNK_SIZE_Z));
        const glm::vec3 maximum(float((id.coord.x + 1) * CHUNK_SIZE_X), float(it->second->vertical_render_extent()),
                                float((id.coord.z + 1) * CHUNK_SIZE_Z));
        if (!detail_chunk_visible(frustum, minimum, maximum, item.depth)) continue;
        chunkRenderer_.draw_foliage(id, commandBuffer);
    }
}

void WorldRenderer::draw_water(VkCommandBuffer commandBuffer, const Frustum& frustum,
                               const glm::vec3& cameraPosition) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    detailCamera_ = cameraPosition;
    // B.4: water is a blended queue — back-to-front via the queue core (this
    // replaces the previous inline std::sort with the canonical ordering engine).
    drawQueues_->clear();
    collect_chunks_into_queue(frustum, Engine::Rendering::DrawQueue::Water);
    farTerrain_.draw_water(commandBuffer);
    for (const auto& item : drawQueues_->sorted(Engine::Rendering::DrawQueue::Water)) {
        chunkRenderer_.draw_water(decode_chunk_id(item.payload), commandBuffer);
    }
}
