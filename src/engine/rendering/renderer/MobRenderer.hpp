#pragma once

#include "Frustum.hpp"
#include "VulkanTypes.hpp"
#include "engine/entity/IEntityWorld.hpp"
#include "engine/entity/IMobBehavior.hpp"

#include <array>

struct LimbMesh {
    AllocatedBuffer buffer;
    uint32_t vertexCount{0};
    glm::vec3 pivotOffset{0.0f};
};

class MobRenderer final {
public:
    void init(VkDevice device, VmaAllocator allocator);
    // Draws every entity carrying the mob component (kMobComponentType, JSON
    // MobSpec blob: typeIndex/yaw/walkAnimProgress/fuseTimer) from the public
    // entity layer — FALTANTES item 11 removed the legacy MobManager track.
    void draw(const engine::entity::IEntityWorld& entities, VkCommandBuffer commandBuffer,
              VkPipelineLayout pipelineLayout, const glm::mat4& viewProjection,
              const Frustum& frustum, const glm::vec3& cameraPosition,
              const glm::vec3& sunDirection, const glm::vec3& sunColor,
              const glm::vec4& environment);
    void cleanup(VkDevice device, VmaAllocator allocator);

private:
    std::array<std::array<LimbMesh, 6>, 6> sharedLimbMeshes_{};
    // L28 (reabertura): indirect draw no caminho de submissão dos mobs. Os
    // parâmetros de draw (VkDrawIndirectCommand) vêm de um buffer GPU-visível
    // construído por frame a partir do culling real (frustum) — a cena voxel
    // deixa de depender só de partículas para indirect.
    VkDevice device_{ VK_NULL_HANDLE };
    VmaAllocator allocator_{ VK_NULL_HANDLE };
    VkBuffer indirectBuffer_{ VK_NULL_HANDLE };
    VmaAllocation indirectAllocation_{ VK_NULL_HANDLE };
    static constexpr std::uint32_t kIndirectCommandCapacity = 6u * 1024u;  // 6 limbs * mobs
    LimbMesh build_box_limb(VkDevice device, VmaAllocator allocator,
                            int texU, int texV, int texW, int texH,
                            float minX, float minY, float minZ,
                            float dx, float dy, float dz,
                            glm::vec3 pivot, float layerIndex, glm::vec4 baseColor);
    void build_mob_limb_meshes(VkDevice device, VmaAllocator allocator);
};
