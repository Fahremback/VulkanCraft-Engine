#pragma once

#include "Frustum.hpp"
#include "VulkanTypes.hpp"
#include "WorldRenderBridge.hpp"
#include "ChunkRenderer.hpp"
#include "FarTerrain.hpp"
#include "ThreadPool.hpp"
#include "engine/rendering/ILumenScene.hpp"
#include "engine/rendering/ISceneRenderQueues.hpp"
#include "engine/rendering/ISceneCulling.hpp"

class World;

class WorldRenderer final : public WorldRenderBridge {
public:
    explicit WorldRenderer(World& world)
        : world_(world), drawQueues_(Engine::Rendering::create_scene_render_queues()) {}

    void configure(VkDevice device, VmaAllocator allocator);
    void begin_frame() override;
    void request_far_terrain(int centerChunkX, int centerChunkZ, int reachChunks,
                             float endpointQuality) override;
    void retire_chunk(ChunkId chunk) override;
    void upload_chunk(ChunkMeshResult result) override;
    void cleanup(bool deviceAlreadyIdle = false);

    // A.3: binds the Lumen-style surface cache this renderer feeds. The
    // mesh->surface pass runs inside upload_chunk (real chunk meshes), so the
    // scene is updated incrementally exactly as chunks stream in/out.
    void set_lumen_scene(Engine::Rendering::ILumenScene* scene) { lumenScene_ = scene; }

    [[nodiscard]] int represented_reach_chunks() const { return farTerrain_.represented_reach_chunks(); }
    [[nodiscard]] int clipmap_level_count() const { return farTerrain_.clipmap_level_count(); }
    [[nodiscard]] float last_build_milliseconds() const { return farTerrain_.last_build_milliseconds(); }
    [[nodiscard]] float applied_endpoint_percent() const { return farTerrain_.applied_endpoint_percent(); }
    [[nodiscard]] bool is_building() const { return farTerrain_.is_building(); }
    void draw_far_surface_shadow(VkCommandBuffer commandBuffer) { farTerrain_.draw_surface_shadow(commandBuffer); }
    void draw_far_shadow(VkCommandBuffer commandBuffer) { farTerrain_.draw_shadow(commandBuffer); }

    void draw_far_surface(VkCommandBuffer commandBuffer);
    void draw(VkCommandBuffer commandBuffer, const Frustum& frustum);
    void draw_details(VkCommandBuffer commandBuffer, const Frustum& frustum);
    void draw_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius = 7);
    void draw_foliage_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius = 7);
    void draw_grass_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius = 7);
    void draw_grass(VkCommandBuffer commandBuffer, const Frustum& frustum);
    void draw_foliage(VkCommandBuffer commandBuffer, const Frustum& frustum);
    void draw_water(VkCommandBuffer commandBuffer, const Frustum& frustum, const glm::vec3& cameraPosition);
    // B.4: sets the camera position used to order the detail queues (opaque
    // front-to-back for early-z, foliage/grass front-to-back, water
    // back-to-front). Called by the scene pass once per frame before the
    // detail draws; the ordering engine is ISceneRenderQueues.
    void set_detail_camera(const glm::vec3& cameraPosition) { detailCamera_ = cameraPosition; }

    // B.4: binds the PUBLIC scene-culling core (ISceneCulling) so the real
    // submission path culls through the deterministic SDK algorithm instead of
    // a private inline copy: extractFrustum/aabbVisible for chunk visibility,
    // occluded() for conservative detail-queue culling and selectLodHysteretic
    // for per-chunk detail tier. Never null after configure(); the renderer
    // falls back to the inline test only when unset.
    void set_scene_culling(Engine::Rendering::ISceneCulling* culling) { sceneCulling_ = culling; }
    // B.4: the view-projection used by the conservative occlusion test (the
    // same mvp the scene pass records with). Stored alongside the detail camera.
    void set_detail_view_proj(const glm::mat4& viewProj) { detailViewProj_ = viewProj; }

    // B.4: observable counts from the last draw — visible chunks, chunks
    // rejected by the SDK frustum, detail chunks rejected by conservative
    // occlusion and the LOD tier split selected by selectLodHysteretic.
    [[nodiscard]] std::uint32_t last_visible_chunks() const noexcept { return visibleChunkCount_; }
    [[nodiscard]] std::uint32_t last_culled_chunks() const noexcept { return culledChunkCount_; }
    [[nodiscard]] std::uint32_t last_occluded_detail_chunks() const noexcept { return occludedDetailCount_; }
    [[nodiscard]] std::uint32_t last_lod_split() const noexcept { return lodSplit_; }

private:
    void draw_details_unlocked(VkCommandBuffer commandBuffer, const Frustum& frustum);
    // B.4: culls the uploaded stable-frontier chunks against `frustum` and
    // pushes the visible ones into drawQueues_ under `queue` with camera-space
    // depth (encoded ChunkId payload). The caller drains sorted(queue). When
    // sceneCulling_ is bound, the visibility test runs through the public
    // ISceneCulling core (extractFrustum planes + aabbVisible) and the counts
    // feed the observable title contract.
    void collect_chunks_into_queue(const Frustum& frustum,
                                   Engine::Rendering::DrawQueue queue);
    // B.4: detail queues (grass/foliage) skip chunks the conservative occlusion
    // test proves hidden behind a nearer opaque chunk; returns true when the
    // chunk should be drawn.
    bool detail_chunk_visible(const Frustum& frustum, const glm::vec3& minimum,
                              const glm::vec3& maximum, float depth) const;
    // A.3: samples the just-uploaded chunk mesh into LumenSurface cards for the
    // bound ILumenScene (bounded per-chunk budget, real world albedo/emission).
    void feed_lumen_scene(const ChunkMeshResult& result);
    World& world_;
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    ChunkRenderer chunkRenderer_;
    FarTerrain farTerrain_;
    ThreadPool farTerrainThreadPool_{1};
    std::vector<AllocatedBuffer> retiredBuffers_[FRAME_OVERLAP];
    uint64_t gpuEpoch_{0};
    Engine::Rendering::ILumenScene* lumenScene_{ nullptr };
    Engine::Rendering::ISceneRenderQueues* queues() const noexcept { return drawQueues_.get(); }
    std::unique_ptr<Engine::Rendering::ISceneRenderQueues> drawQueues_;
    Engine::Rendering::ISceneCulling* sceneCulling_{ nullptr };
    glm::vec3 detailCamera_{ 0.0f, 0.0f, 0.0f };
    glm::mat4 detailViewProj_{ 1.0f };
    // B.4 observable per-draw counters (fed by the SDK culling core).
    mutable std::uint32_t visibleChunkCount_{ 0 };
    mutable std::uint32_t culledChunkCount_{ 0 };
    mutable std::uint32_t occludedDetailCount_{ 0 };
    mutable std::uint32_t lodSplit_{ 0 };
};
