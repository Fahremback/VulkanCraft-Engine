#pragma once

#include "engine/rendering/MaterialGraph.hpp"
#include "engine/rendering/RenderGraph.hpp"
#include "engine/rendering/materials/Material.hpp"
#include "engine/rendering/vulkan/VulkanTypes.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Rendering {

// ─── Material Graph → GLSL ───
// Compiles a material graph's IR into a real GLSL fragment shader source that
// can be fed to glslc/shaderc. The generated shader exposes material parameters
// as uniforms in a single UBO and outputs BaseColor/Roughness/Metallic/Emissive.
struct GlslGenerationResult {
    std::string source;
    std::vector<std::string> uniformNames;
    std::vector<MaterialValueType> uniformTypes;
    // Binding of the LightParams UBO inside the generated fragment shader
    // (declared after the texture samplers so it never collides with them).
    uint32_t lightUboBinding{ 1 };
    // Binding of the optional shadow-map sampler (declared after the light
    // UBO). Consumers without shadows bind a dummy texture and disable the
    // shadow term via LightUboData::shadowParams.x.
    uint32_t shadowSamplerBinding{ 2 };
    std::vector<MaterialGraphError> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return errors.empty(); }
};

// ─── Real lighting (LightComponent → renderer) ───
// The generated material shader consumes a single LightParams UBO (std140)
// written by the caller from the scene's LightComponents: one directional
// light (sun) plus a small array of point lights. Matches the GLSL block
// emitted by material_graph_to_glsl — keep the two layouts in sync.
inline constexpr uint32_t kMaxPointLights = 8;
inline constexpr uint32_t kMaxSpotLights = 4;
inline constexpr uint32_t kMaxAreaLights = 4;
inline constexpr uint32_t kShadowCascadeCount = 4;

struct LightUboData {
    glm::vec4 cameraPosition;              // xyz = camera position
    glm::vec4 sunDirection;                // xyz = normalized direction, w = enabled
    glm::vec4 sunColor;                    // rgb = color * intensity
    glm::mat4 sunViewProj;                 // light view-projection (cascade 0 / single-map fallback)
    glm::vec4 shadowParams;                // x = enabled, y = depth bias, z = cascade count (0 = single map)
    glm::vec4 pointLightPos[kMaxPointLights];    // xyz = position, w = range
    glm::vec4 pointLightColor[kMaxPointLights];  // rgb = color * intensity, w = enabled
    glm::vec4 spotLightPos[kMaxSpotLights];      // xyz = position, w = range
    glm::vec4 spotLightDir[kMaxSpotLights];      // xyz = direction, w = enabled
    glm::vec4 spotLightParams[kMaxSpotLights];   // x = cos(inner/2), y = cos(outer/2)
    glm::vec4 spotLightColor[kMaxSpotLights];    // rgb = color * intensity
    glm::vec4 areaLightPos[kMaxAreaLights];      // xyz = center, w = enabled
    glm::vec4 areaLightNormal[kMaxAreaLights];   // xyz = facing normal
    glm::vec4 areaLightHalf[kMaxAreaLights];     // x = halfWidth, y = halfHeight
    glm::vec4 areaLightColor[kMaxAreaLights];    // rgb = color * intensity
    glm::mat4 sunCascadeVP[kShadowCascadeCount]; // per-cascade light view-projection (atlas 2x2)
    glm::vec4 sunCascadeSplits;            // xyz = view-depth split points, w = cascade count
    glm::vec4 cameraForward;               // xyz = camera forward (cascade selection)
};
static_assert(sizeof(LightUboData) == 16 * 3 + 64 + 16 + 16 * kMaxPointLights * 2 +
                                    16 * kMaxSpotLights * 4 + 16 * kMaxAreaLights * 4 +
                                    64 * kShadowCascadeCount + 16 * 2);

// Push constant shared by the material vertex shader (editor_material.vert).
struct MaterialPushConstants {
    glm::mat4 mvp;   // proj * view * model
    glm::mat4 model; // world transform
};
static_assert(sizeof(MaterialPushConstants) == 128);

[[nodiscard]] GlslGenerationResult material_graph_to_glsl(const MaterialGraph& graph);

// A standard PBR material graph (Albedo/Roughness/Metallic/Emissive exposed as
// UBO parameters) derived from a MaterialAsset — used by editor and game to
// render meshes through the material-graph pipeline.
[[nodiscard]] MaterialGraph material_graph_from_pbr(const MaterialAsset& material);

// Compiles GLSL to SPIR-V via glslc (must be on PATH). Returns an empty vector
// on failure. Shared by the editor viewport and the game renderer.
[[nodiscard]] std::vector<uint32_t> compile_glsl_to_spirv(const std::string& source,
                                                          VkShaderStageFlagBits stage);

// ─── Render Graph → Vulkan executor ───
// Executes a compiled RenderGraph on real Vulkan resources. The graph's
// compiled pass order drives the command stream: each pass begins its real
// render pass on a caller-provided framebuffer, runs the pass's draw callback,
// and ends; the compiled barrier list is emitted between passes. Resource
// ownership stays with the caller (renderer), which registers the Vulkan
// objects (render pass + per-swapchain-image framebuffers) for every pass.
class VulkanRenderGraphExecutor final {
public:
    VulkanRenderGraphExecutor() = default;
    ~VulkanRenderGraphExecutor() = default;

    VulkanRenderGraphExecutor(const VulkanRenderGraphExecutor&) = delete;
    VulkanRenderGraphExecutor& operator=(const VulkanRenderGraphExecutor&) = delete;

    bool initialize(VkDevice device, const RenderGraph& graph,
                    std::string* error = nullptr);
    void shutdown();

    [[nodiscard]] bool valid() const noexcept { return initialized_; }
    [[nodiscard]] const RenderGraphCompileResult& compile_result() const noexcept { return compileResult_; }
    [[nodiscard]] std::size_t executed_pass_count() const noexcept { return executedPassCount_; }
    [[nodiscard]] std::uint64_t total_barriers() const noexcept { return totalBarriers_; }
    [[nodiscard]] std::string last_error() const noexcept { return lastError_; }

    // Per-pass real Vulkan frame data: the render pass to execute, one
    // framebuffer per swapchain image (index = acquired image), the clear
    // values matching the render pass attachments, and a callback recording
    // the pass's actual content (may be empty for a clear-only pass).
    struct PassFrame {
        VkRenderPass renderPass{ VK_NULL_HANDLE };
        std::vector<VkFramebuffer> framebuffers;
        std::vector<VkClearValue> clearValues;
        std::function<void(VkCommandBuffer)> draw;
    };
    void register_pass(RenderPassId pass, PassFrame frame);
    void unregister_pass(RenderPassId pass);

    // Records every enabled pass in compiled order into commandBuffer,
    // inserting the compiled barriers between passes. swapImageIndex selects
    // the per-swapchain-image framebuffer of each pass. The caller submits.
    void record(VkCommandBuffer commandBuffer, uint32_t swapImageIndex,
                VkExtent2D extent) const;

private:
    VkDevice device_{VK_NULL_HANDLE};
    bool initialized_{false};
    RenderGraphCompileResult compileResult_;
    std::unordered_map<RenderPassId, PassFrame> passFrames_;
    mutable std::size_t executedPassCount_{0};
    mutable std::uint64_t totalBarriers_{0};
    std::string lastError_;
};

// ─── Shader / asset hot reload watcher ───
// Monitors files by path + size + mtime; poll() reports changed files so the
// renderer can reimport assets and rebuild pipelines without restarting.
struct FileStamp {
    std::filesystem::path path;
    std::uintmax_t size{};
    std::filesystem::file_time_type modified{};
};

class FileWatcher final {
public:
    explicit FileWatcher(std::chrono::milliseconds interval = std::chrono::milliseconds(250));
    void watch(const std::filesystem::path& path);
    void unwatch(const std::filesystem::path& path);
    // Returns files whose stamp changed since last poll (or newly watched).
    [[nodiscard]] std::vector<std::filesystem::path> poll();
    [[nodiscard]] std::size_t watched_count() const noexcept { return stamps_.size(); }

private:
    std::chrono::milliseconds interval_;
    std::map<std::filesystem::path, FileStamp> stamps_;
};

} // namespace Engine::Rendering
