#include "VulkanGame.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/Components.hpp"
#include "engine/assets/GltfGeometry.hpp"
#include "engine/assets/GltfAssets.hpp"
#include "engine/assets/RuntimePackage.hpp"
#include "engine/rendering/vulkan/MaterialPipeline.hpp"
#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/Ragdoll.hpp"
#include "engine/animation/AnimationRuntime.hpp"
#include "engine/scripting/ScriptRuntime.hpp"
#include "engine/audio/AudioRuntime.hpp"
#include "engine/gameplay/WeaponSystem.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <chrono>
#include <optional>
#include <set>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <algorithm>

using namespace Engine;
using namespace Engine::Rendering;

void VulkanGame::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data,
                              VkBuffer& buffer, VmaAllocation& allocation) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create GPU buffer");
    void* mapped = nullptr;
    vmaMapMemory(allocator, allocation, &mapped);
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vmaUnmapMemory(allocator, allocation);
}

void VulkanGame::drawMeshResource(VkCommandBuffer cb, const GameMeshResource& mesh,
                                  const glm::mat4& mvp, const glm::mat4& model) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vb, &offset);
    if (mesh.ib != VK_NULL_HANDLE) vkCmdBindIndexBuffer(cb, mesh.ib, 0, VK_INDEX_TYPE_UINT32);
    const Rendering::MaterialPushConstants push{ mvp, model };
    vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    for (const MeshRange& range : mesh.ranges) {
        if (range.indexed) vkCmdDrawIndexed(cb, range.indexCount, 1, range.firstIndex, 0, 0);
        else vkCmdDraw(cb, range.indexCount, 1, range.vertexOffset, 0);
    }
}

