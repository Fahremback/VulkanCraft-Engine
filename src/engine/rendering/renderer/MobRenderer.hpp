#pragma once

#include "Frustum.hpp"
#include "Mob.hpp"
#include "VulkanTypes.hpp"

#include <array>

struct LimbMesh {
    AllocatedBuffer buffer;
    uint32_t vertexCount{0};
    glm::vec3 pivotOffset{0.0f};
};

class MobRenderer final {
public:
    void init(VkDevice device, VmaAllocator allocator);
    void draw(const MobManager& mobManager, VkCommandBuffer commandBuffer,
              VkPipelineLayout pipelineLayout, const glm::mat4& viewProjection,
              const Frustum& frustum, const glm::vec3& cameraPosition,
              const glm::vec3& sunDirection, const glm::vec3& sunColor,
              const glm::vec4& environment);
    void cleanup(VkDevice device, VmaAllocator allocator);

private:
    std::array<std::array<LimbMesh, 6>, 6> sharedLimbMeshes_{};
    LimbMesh build_box_limb(VkDevice device, VmaAllocator allocator,
                            int texU, int texV, int texW, int texH,
                            float minX, float minY, float minZ,
                            float dx, float dy, float dz,
                            glm::vec3 pivot, float layerIndex, glm::vec4 baseColor);
    void build_mob_limb_meshes(VkDevice device, VmaAllocator allocator);
};
