#include "EditorApplication.hpp"
#include "BlockTextureAtlas.hpp"
// Frontend port from the Wicked Engine Editor (MIT, commit 2aa9fdf…): Font
// Awesome 6 icon font + codepoint macros, and the Liberation Sans UI font
// (zstd-compressed, same font Wicked ships). See frontend/PORTS.md.
#include "engine/compression/ICompressionProvider.hpp"
#include "engine/editor/IFileWatcher.hpp"
#include "engine/editor/IFileChangeDebounce.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include "../engine/physics/VoxelBoxMerger.hpp"
#include "../engine/animation/AnimationAssets.hpp"
#include "../engine/rendering/vulkan/MaterialPipeline.hpp"
#include "../engine/audio/AudioRuntime.hpp"
#include "../engine/audio/OggDecoder.hpp"
#include "../engine/gameplay/DialogueSystem.hpp"
#include "../engine/gameplay/DestructionRuntime.hpp"
#include "../engine/gameplay/MissionSystem.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include <array>
#include <map>
#include <random>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <fstream>
#include <chrono>
#include <ctime>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>

// Local copies of the file-scope render helpers (the originals live in a
// nested anonymous namespace inside `namespace Engine`, later in this TU, so
// they are not visible to the runtime-wired Wicked-port code below). These
// global-scope versions share the same implementations.
namespace {
glm::mat4 model_from_transform(const Engine::TransformComponent& t) {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (!finite(t.position.x) || !finite(t.position.y) || !finite(t.position.z) ||
        !finite(t.rotation.x) || !finite(t.rotation.y) || !finite(t.rotation.z) ||
        !finite(t.scale.x) || !finite(t.scale.y) || !finite(t.scale.z)) {
        // NaN/inf guard: a non-finite transform would poison the MVP matrix and
        // black out the viewport. Draw this entity at the origin instead.
        return glm::mat4(1.0f);
    }
    glm::mat4 model(1.0f);
    model = glm::translate(model, t.position);
    model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
    model = glm::scale(model, t.scale);
    return model;
}
// Current fog state (set per-frame from WeatherComponent)
static glm::vec4 g_fogParams{ 0.001f, 100.0f, 0.0f, 0.0f };
static glm::vec4 g_fogColor{ 0.5f, 0.6f, 0.7f, 1.0f };
void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& mvp,
                    const glm::vec4& color, const glm::mat4& model = glm::mat4(1.0f)) {
    const Engine::ScenePushConstants pc{ mvp, color, g_fogParams, g_fogColor, model };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(pc)), &pc);
}
void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color,
                       const glm::mat4& model = glm::mat4(1.0f)) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color, model);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}
void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb,
                              const VkBuffer& ib, uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color, const glm::mat4& model = glm::mat4(1.0f)) {
    draw_indexed_cube(cmd, layout, vb, ib, indexCount, mvp, color, model);
}

} // namespace

#include <imgui_impl_vulkan.h>
#include <VkBootstrap.h>
#include <miniaudio.h>
#include <thread>

// ---------------------------------------------------------------------------
// Safe Vulkan helpers (avoid null-pointer dereferences on mapping failure)
// ---------------------------------------------------------------------------
static bool safe_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                             VkDeviceSize size, VkFlags flags, void** ppData) {
    *ppData = nullptr;
    const VkResult result = vkMapMemory(device, memory, offset, size, flags, ppData);
    if (result != VK_SUCCESS) {
        std::cerr << "[Vulkan] vkMapMemory failed (" << result << ")" << std::endl;
        return false;
    }
    return true;
}
static bool safe_map_and_copy(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                              VkDeviceSize size, const void* source) {
    void* data = nullptr;
    if (!safe_vkMapMemory(device, memory, offset, size, 0, &data)) return false;
    std::memcpy(data, source, static_cast<size_t>(size));
    vkUnmapMemory(device, memory);
    return true;
}

// PlayNavAgent — the public provider's path follower (mirrors the legacy
// NavigationAgent stepping so the Fase 8 behavior is preserved).
void PlayNavAgent::set_path(std::vector<glm::vec3> points) {
    path = std::move(points);
    waypoint = 0;
    reached = path.empty();
}

void PlayNavAgent::update(float deltaTime) {
    if (reached) return;
    if (waypoint >= path.size()) {
        reached = true;
        return;
    }
    float remaining = speed * deltaTime;
    while (remaining > 0.0f && waypoint < path.size()) {
        const glm::vec3 target = path[waypoint];
        const glm::vec3 dir = target - position;
        const float dist = glm::length(dir);
        if (dist <= stoppingDistance) {
            position = target;
            ++waypoint;
            continue;
        }
        const float step = std::min(remaining, dist);
        position += (dir / dist) * step;
        remaining -= step;
        if (step >= dist - 1e-4f) {
            position = target;
            ++waypoint;
        }
    }
    if (waypoint >= path.size()) reached = true;
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <wincodec.h>
#include <propsys.h> // IPropertyBag2 (WIC frame encode)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace Engine {


void EditorApplication::register_project_templates() {
    // Built-in project templates for the wizard (ezEngine pillar). The wizard
    // lists these; create_project_from_template materializes the scaffold.
    Engine::Editor::register_builtin_templates(m_templateRegistry);
            if (playScene) {
                const auto tit = playScene->transformComponents.find(m_playTestEntityId);
                if (tit != playScene->transformComponents.end()) {
                    y = tit->second.position.y;
                    fell = y < 4.0f;
                }
            }
            std::cout << "[Editor] PLAY_TEST " << (fell ? "PASS" : "FAIL")
                      << " (cube y=" << y << ")" << std::endl;
            vkDeviceWaitIdle(m_device);
            std::exit(fell ? 0 : 1);
        }

        if (m_materialTestFramesLeft > 0 && --m_materialTestFramesLeft == 0) {
            const auto it = m_graphMaterialPipelines.find(m_materialTestMatId);
            bool ok = false;
            std::string error;
            if (it != m_graphMaterialPipelines.end()) {
                ok = it->second.valid;
                error = it->second.lastError;
            }
            // Texture path: rebuild the live preview graph with a TextureSample
            // feeding BaseColor and verify a texture pipeline builds and binds.
            UUID textureId;
            std::string textureName;
            for (const AssetMetadata& meta : m_assetRegistry.snapshot()) {
                if (meta.type == AssetType::Texture && meta.isCooked) {
                    textureId = meta.id;
                    textureName = meta.sourcePath.filename().string();
                    break;
                }
            }
            if (ok && textureId.is_valid()) {
                auto& live = m_specializedEditors.live_material_graph_mutable();
                const auto texNode = live.add_texture_sample("Test Texture");
                if (auto* node = live.find_node(texNode)) node->value = textureId.to_string();
                for (const auto& candidate : live.nodes()) {
                    if (candidate.kind != Rendering::MaterialNodeKind::Output ||
                        candidate.parameter != "BaseColor")
                        continue;
                    auto* outNode = live.find_node(candidate.id);
                    if (!outNode || outNode->inputs.empty()) continue;
                    const auto* src = outNode->inputs[0].source != Rendering::InvalidMaterialNode
                        ? live.find_node(outNode->inputs[0].source) : nullptr;
                    if (src && src->kind == Rendering::MaterialNodeKind::Constant) {
                        (void)live.connect(texNode, outNode->id, 0);
                        break;
                    }
                }
                m_liveGraphHash = 0;
                destroy_graph_pipeline(m_liveGraphPipeline);
                if (!build_graph_pipeline(live, m_liveGraphPipeline)) {
                    ok = false;
                    error = m_liveGraphPipeline.lastError;
                } else if (m_liveGraphPipeline.textures.empty()) {
                    ok = false;
                    error = "texture pipeline has no bound textures";
                } else {
                    error = "texture: " + textureName;
                }
            } else if (ok) {
                ok = false;
                error = (error.empty() ? "" : error + "; ") + "no cooked texture asset found";
            }
            std::cout << "[Editor] MATERIAL_TEST " << (ok ? "PASS" : "FAIL")
                      << (error.empty() ? "" : " (" + error + ")") << std::endl;
            vkDeviceWaitIdle(m_device);
            std::exit(ok ? 0 : 1);
        }
    }

    // Final autosave flush: the window is closing — persist any change that
    // arrived inside the debounce window so nothing is lost on exit.
    autosave_scene(true);

    vkDeviceWaitIdle(m_device);
}







} // namespace



        destroy_mesh_resources();
        destroy_graph_material_pipelines();
        destroy_asset_thumbnails();
        destroy_block_cube();
        destroy_thumbnail_target();
        destroy_voxel_editor_meshes();
        cleanup_offscreen_target();

        if (m_offscreen.renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_offscreen.renderPass, nullptr);
            m_offscreen.renderPass = VK_NULL_HANDLE;
        }
        if (m_offscreen.pickRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_offscreen.pickRenderPass, nullptr);
            m_offscreen.pickRenderPass = VK_NULL_HANDLE;
        }
        if (m_offscreen.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, m_offscreen.sampler, nullptr);
            m_offscreen.sampler = VK_NULL_HANDLE;
        }

        destroy_buffer(m_cubeVB);
        destroy_buffer(m_cubeIB);
        destroy_buffer(m_lightIconVB);
        destroy_buffer(m_cameraIconVB);
        destroy_buffer(m_gizmoVB);
        destroy_buffer(m_gizmoIB);
        destroy_buffer(m_terrainVB);
        destroy_buffer(m_terrainIB);
        m_terrainValid = false;
        // Runtime-wired Wicked-port resources.
        destroy_buffer(m_envSphereVB);
        destroy_buffer(m_envSphereIB);
        destroy_buffer(m_decalVB);
        destroy_buffer(m_decalIB);
        for (auto& [id, sim] : m_hairs) { (void)id; destroy_buffer(sim.vb); }
        m_hairs.clear();
        for (auto& [id, sim] : m_softBodies) { (void)id; destroy_buffer(sim.vb); destroy_buffer(sim.ib); }
        m_softBodies.clear();
        for (auto& [id, cloud] : m_splatClouds) { (void)id; destroy_buffer(cloud.vb); }
        m_splatClouds.clear();
        for (auto& [id, pb] : m_paintBuffers) { (void)id; destroy_buffer(pb.vb); }
        m_paintBuffers.clear();
        if (m_envCapture.valid) {
            for (int i = 0; i < 6; ++i) {
                if (m_envCapture.framebuffers[i]) vkDestroyFramebuffer(m_device, m_envCapture.framebuffers[i], nullptr);
                if (m_envCapture.views[i]) vkDestroyImageView(m_device, m_envCapture.views[i], nullptr);
            }
            if (m_envCapture.image) vkDestroyImage(m_device, m_envCapture.image, nullptr);
            if (m_envCapture.memory) vkFreeMemory(m_device, m_envCapture.memory, nullptr);
            if (m_envCapture.renderPass) vkDestroyRenderPass(m_device, m_envCapture.renderPass, nullptr);
            if (m_envCapture.sampler) vkDestroySampler(m_device, m_envCapture.sampler, nullptr);
        }
        if (m_envCubeView) vkDestroyImageView(m_device, m_envCubeView, nullptr);
        if (m_envDepthView) vkDestroyImageView(m_device, m_envDepthView, nullptr);
        if (m_envDepthImage) vkDestroyImage(m_device, m_envDepthImage, nullptr);
        if (m_envDepthMemory) vkFreeMemory(m_device, m_envDepthMemory, nullptr);
        if (m_envSphereDescLayout) vkDestroyDescriptorSetLayout(m_device, m_envSphereDescLayout, nullptr);
        if (m_envSphereDescPool) vkDestroyDescriptorPool(m_device, m_envSphereDescPool, nullptr);
        if (m_envSpherePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_envSpherePipeline, nullptr);
        if (m_envSpherePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_envSpherePipelineLayout, nullptr);
        if (m_envSphereFragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_envSphereFragShader, nullptr);
        if (m_envSphereVertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_envSphereVertShader, nullptr);
        if (m_splatPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_splatPipeline, nullptr);
        if (m_splatPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_splatPipelineLayout, nullptr);
        if (m_splatFragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_splatFragShader, nullptr);
        if (m_splatVertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_splatVertShader, nullptr);
        if (m_hairPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_hairPipeline, nullptr);

        if (m_pickPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_pickPipeline, nullptr); m_pickPipeline = VK_NULL_HANDLE; }
        if (m_gizmoPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_gizmoPipeline, nullptr); m_gizmoPipeline = VK_NULL_HANDLE; }
        if (m_wireframePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_wireframePipeline, nullptr); m_wireframePipeline = VK_NULL_HANDLE; }
        if (m_scenePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_scenePipeline, nullptr); m_scenePipeline = VK_NULL_HANDLE; }
        if (m_scenePipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_scenePipelineLayout, nullptr); m_scenePipelineLayout = VK_NULL_HANDLE; }
        if (m_gridPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_gridPipeline, nullptr); m_gridPipeline = VK_NULL_HANDLE; }
        destroy_scene_light_resources();
        if (m_gridPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_gridPipelineLayout, nullptr); m_gridPipelineLayout = VK_NULL_HANDLE; }
        if (m_gridFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_gridFragShader, nullptr); m_gridFragShader = VK_NULL_HANDLE; }
        if (m_gridVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_gridVertShader, nullptr); m_gridVertShader = VK_NULL_HANDLE; }
        if (m_pickFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_pickFragShader, nullptr); m_pickFragShader = VK_NULL_HANDLE; }
        if (m_viewportFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_viewportFragShader, nullptr); m_viewportFragShader = VK_NULL_HANDLE; }
        if (m_viewportVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_viewportVertShader, nullptr); m_viewportVertShader = VK_NULL_HANDLE; }

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (m_imguiDescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_imguiDescriptorPool, nullptr);
        }

        for (size_t i = 0; i < 2; i++) {
            vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
            vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
        }

        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        }

        for (auto fb : m_framebuffers) {
            vkDestroyFramebuffer(m_device, fb, nullptr);
        }

        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        }

        for (auto view : m_swapchainViews) {
            vkDestroyImageView(m_device, view, nullptr);
        }

        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        }

        vkDestroyDevice(m_device, nullptr);
        // run() calls cleanup() and the destructor calls it again — reset the
        // device so the second pass is a no-op. Without this, the second pass
        // called vkDeviceWaitIdle on the already-destroyed handle
        // ("vkDeviceWaitIdle: Invalid device" from the loader at shutdown).
        m_device = VK_NULL_HANDLE;
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        vkDestroyInstance(m_instance, nullptr);

        if (m_window) {
            glfwDestroyWindow(m_window);
            glfwTerminate();
        }
    }
}

// ===========================================================================
// File/folder pickers + scene loading (Abrir Jogo / Procurar Pasta).
// ===========================================================================

bool EditorApplication::pick_file_dialog(std::string& outPath, const wchar_t* filter,
                                         const wchar_t* title, const wchar_t* defExt) {
    wchar_t buf[MAX_PATH]{ 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(m_window);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}
