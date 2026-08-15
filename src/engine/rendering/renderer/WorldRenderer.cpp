#include "WorldRenderer.hpp"

#include "World.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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

void WorldRenderer::draw_details_unlocked(VkCommandBuffer commandBuffer, const Frustum& frustum) {
    for (auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        const int cx = key.first;
        const int cz = key.second;
        const glm::vec3 minimum(float(cx * CHUNK_SIZE_X), 0.0f, float(cz * CHUNK_SIZE_Z));
        const glm::vec3 maximum(float((cx + 1) * CHUNK_SIZE_X), float(chunk->vertical_render_extent()),
                                float((cz + 1) * CHUNK_SIZE_Z));
        if (frustum.is_box_visible(minimum, maximum)) chunkRenderer_.draw(chunk->id(), commandBuffer);
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
    for (auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        const glm::vec3 minimum(float(key.first * CHUNK_SIZE_X), 0.0f, float(key.second * CHUNK_SIZE_Z));
        const glm::vec3 maximum(float((key.first + 1) * CHUNK_SIZE_X), float(chunk->vertical_render_extent()),
                                float((key.second + 1) * CHUNK_SIZE_Z));
        if (frustum.is_box_visible(minimum, maximum)) chunkRenderer_.draw_grass(chunk->id(), commandBuffer);
    }
}

void WorldRenderer::draw_foliage(VkCommandBuffer commandBuffer, const Frustum& frustum) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    for (auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        const glm::vec3 minimum(float(key.first * CHUNK_SIZE_X), 0.0f, float(key.second * CHUNK_SIZE_Z));
        const glm::vec3 maximum(float((key.first + 1) * CHUNK_SIZE_X), float(chunk->vertical_render_extent()),
                                float((key.second + 1) * CHUNK_SIZE_Z));
        if (frustum.is_box_visible(minimum, maximum)) chunkRenderer_.draw_foliage(chunk->id(), commandBuffer);
    }
}

void WorldRenderer::draw_water(VkCommandBuffer commandBuffer, const Frustum& frustum,
                               const glm::vec3& cameraPosition) {
    std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
    std::vector<std::pair<float, std::shared_ptr<Chunk>>> visible;
    visible.reserve(world_.chunks.size());
    for (auto& [key, chunk] : world_.chunks) {
        if (chunk->state.load() != ChunkState::Uploaded || !world_.inside_stable_frontier(key)) continue;
        const glm::vec3 minimum(float(key.first * CHUNK_SIZE_X), 0.0f, float(key.second * CHUNK_SIZE_Z));
        const glm::vec3 maximum(float((key.first + 1) * CHUNK_SIZE_X), float(chunk->vertical_render_extent()),
                                float((key.second + 1) * CHUNK_SIZE_Z));
        if (frustum.is_box_visible(minimum, maximum)) {
            const glm::vec3 center = (minimum + maximum) * 0.5f;
            visible.emplace_back(glm::dot(center - cameraPosition, center - cameraPosition), chunk);
        }
    }
    std::sort(visible.begin(), visible.end(), [](const auto& left, const auto& right) { return left.first > right.first; });
    farTerrain_.draw_water(commandBuffer);
    for (const auto& [distance, chunk] : visible) chunkRenderer_.draw_water(chunk->id(), commandBuffer);
}
