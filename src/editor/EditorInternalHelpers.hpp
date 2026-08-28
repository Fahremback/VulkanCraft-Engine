// EditorInternalHelpers.hpp
// Shared file-scope helpers used by multiple EditorApplication*.cpp TUs after
// the monolithic split.  These were originally in anonymous namespaces inside
// the single EditorApplication.cpp; moving them to a header restores visibility.
#pragma once

#include "EditorApplication.hpp"
#include <vector>
#include <cstdint>
#include <type_traits>
#include <cstring>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>

namespace Engine {

inline constexpr int kVoxelSizeX = 32;
inline constexpr int kVoxelSizeY = 24;
inline constexpr int kVoxelSizeZ = 32;

// ── Math / transform ──────────────────────────────────────────────────────
glm::mat4 model_from_transform(const TransformComponent& t);
glm::mat3 rotation_axis_from_y(const glm::vec3& axis);

// ── Vulkan helpers ────────────────────────────────────────────────────────
bool safe_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                      VkDeviceSize size, VkFlags flags, void** ppData);
bool safe_map_and_copy(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                       VkDeviceSize size, const void* source);

void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout,
                    const glm::mat4& mvp, const glm::vec4& color,
                    const glm::mat4& model = glm::mat4(1.0f));
void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h);
void set_viewport_scissor_offset(VkCommandBuffer cmd, float offsetX, float offsetY,
                                 uint32_t w, uint32_t h);
void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout,
                       const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp,
                       const glm::vec4& color,
                       const glm::mat4& model = glm::mat4(1.0f));
void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout,
                              const VkBuffer& vb, const VkBuffer& ib,
                              uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color,
                              const glm::mat4& model = glm::mat4(1.0f));

// ── Gizmo geometry builders ───────────────────────────────────────────────
std::pair<uint32_t, uint32_t> append_transformed_cube(
    std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
    const glm::mat4& model, const glm::vec3& color);
std::pair<uint32_t, uint32_t> append_cone(
    std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
    float baseY, float tipY, float radius, int segments,
    const glm::mat3& rot, const glm::vec3& color);
std::pair<uint32_t, uint32_t> append_ring(
    std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
    const glm::vec3& axis, float radius, int segments,
    const glm::vec3& color);
void build_cube(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices);
std::vector<uint32_t> read_spv(const char* name);
VkPipeline create_scene_pipeline(VkDevice device, VkRenderPass renderPass, VkPipelineLayout layout,
                                 VkShaderModule vert, VkShaderModule frag,
                                 VkSampleCountFlagBits samples, bool wireframe, bool depthTest, bool cull,
                                 bool withUv = false, bool noVertexInput = false, bool blend = false,
                                 bool lessOrEqualDepth = false, bool depthBias = false,
                                 bool depthWrite = true,
                                 VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

// ── Terrain ───────────────────────────────────────────────────────────────
float terrain_surface_height(uint32_t seed, float scale, int octaves,
                              float amount, float falloffParam,
                              float halfExtent, float x, float z);

// ── Material-graph helpers ────────────────────────────────────────────────
uint64_t hash_material_graph(const Rendering::MaterialGraph& graph);
Rendering::MaterialGraph material_graph_from_asset(const MaterialAsset& mat);

// ── Material UBO layout ───────────────────────────────────────────────────
size_t material_std140_size(Rendering::MaterialValueType type);
size_t material_std140_alignment(Rendering::MaterialValueType type);
size_t align_material_offset(size_t offset, size_t alignment);
void write_ubo_value(std::byte* dst, Rendering::MaterialValueType type,
                     const Rendering::MaterialValue& value);

} // namespace Engine
