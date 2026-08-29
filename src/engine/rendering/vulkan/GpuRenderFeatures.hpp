#pragma once

#include "VulkanTypes.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace Engine::Rendering {

// GPU-facing feature contract shared by the real game renderer. All optional
// features are represented by explicit flags; unsupported paths are disabled
// deterministically instead of silently pretending to run.
struct alignas(16) GpuRenderFeatures {
    glm::vec4 gi{0.0f};          // x enabled, y probe weight, z restir weight, w denoise weight
    glm::vec4 reflections{0.0f}; // x SSR, y probe, z roughness cutoff, w water
    glm::vec4 atmosphere{0.0f};  // x daylight, y cloud density, z planet scale, w exposure
    glm::vec4 temporal{0.0f};    // x history valid, y motion x, z motion y, w disocclusion
    glm::vec4 debug{0.0f};       // x mode, y cards, z probes, w tracing
    glm::vec4 fluids{0.0f};      // x enabled, y level, z flow x, w flow z
    glm::vec4 vfx{0.0f};         // x enabled, y particle count, z hair, w xr
    glm::vec4 material{0.0f};    // x data-driven, y emissive scale, z roughness, w metallic
    glm::vec4 debugCounts{0.0f}; // x cards, y probes, z captures, w temporal confidence
    glm::vec4 extent{0.0f};      // x render width, y render height (z/w unused) — the
                                 // temporal history is laid out row-major with a
                                 // stride of width, so the compute shader needs the
                                 // REAL width instead of a hardcoded 8192.
};

static_assert(std::is_standard_layout_v<GpuRenderFeatures>);
static_assert(sizeof(GpuRenderFeatures) == sizeof(glm::vec4) * 10);

struct GpuFeatureBinding;
void create_gpu_feature_binding(VkDevice device, VmaAllocator allocator,
                                GpuFeatureBinding& out);
void update_gpu_feature_binding(VmaAllocator allocator, GpuFeatureBinding& binding,
                                const GpuRenderFeatures& features);
void destroy_gpu_feature_binding(VkDevice device, VmaAllocator allocator,
                                 GpuFeatureBinding& binding);

struct GpuFeatureBinding {
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    VkDescriptorPool pool{VK_NULL_HANDLE};
    VkDescriptorSet set{VK_NULL_HANDLE};
    VkBuffer uniformBuffer{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};
    void* mapped{nullptr};
    VkDeviceSize size{sizeof(GpuRenderFeatures)};
};

} // namespace Engine::Rendering
