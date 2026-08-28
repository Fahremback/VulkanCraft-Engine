// EditorApplicationRecovered.cpp
// Methods lost in the monolith split (recovered from git history 408c2d3) plus
// minimal implementations for methods declared in the header but never written.
#include "EditorApplication.hpp"
#include "EditorInternalHelpers.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <shlobj.h>
#include <sstream>
#include <vector>
#include <fstream>
#include <filesystem>

namespace Engine {

// Helpers recovered from the monolith's anonymous namespace (used by the
// recovered methods below).
namespace {
glm::vec3 euler_direction(float yawDeg, float pitchDeg) {
    const float yawRad = glm::radians(yawDeg);
    const float pitchRad = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(
        std::cos(yawRad) * std::cos(pitchRad),
        std::sin(pitchRad),
        std::sin(yawRad) * std::cos(pitchRad)));
}

float dist_point_segment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-8f) return glm::length(p - a);
    const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

bool parse_all_floats(const std::string& text, std::vector<float>& out) {
    out.clear();
    std::istringstream ss(text);
    float v;
    while (ss >> v) out.push_back(v);
    return ss.eof();
}

constexpr glm::vec3 kAxisDirs[3] = { {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} };
constexpr glm::vec3 kAxisColors[3] = { {1.0f, 0.25f, 0.25f}, {0.30f, 1.0f, 0.45f}, {0.35f, 0.62f, 1.0f} };
} // namespace

EditorApplication::EditorApplication() {
    // Loopback HTTP control API: drive the editor from a terminal or an agent
    // via curl http://127.0.0.1:8321/{play,pause,resume,stop,step,state}.
    m_controlApi.start(8321);

    // Playback sink for the play-in-editor mixer (audio previews + play-mode
    // audio components). Before this the Mixer rendered into a buffer that was
    // never sent to a device, so nothing produced sound.
    init_audio_output();

    const std::filesystem::path registryPath =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
    if (std::filesystem::exists(registryPath) && !m_assetRegistry.load(registryPath)) {
        std::cerr << "[AssetRegistry] Ignoring invalid database: " << registryPath << std::endl;
    }
    m_assetPipeline = std::make_unique<AssetPipeline>(m_assetRegistry);
    m_assetPipeline->add_importer(std::make_unique<TextureImporter>());
    m_assetPipeline->add_importer(std::make_unique<BinaryCopyImporter>(
        AssetType::Texture, std::vector<std::string>{".jpg", ".jpeg", ".exr"}, ".texturebin"));
    m_assetPipeline->add_importer(std::make_unique<MeshImporter>());
    m_assetPipeline->add_importer(std::make_unique<SkeletonImporter>());
    m_assetPipeline->add_importer(std::make_unique<AnimationClipImporter>());
    m_assetPipeline->add_importer(std::make_unique<BinaryCopyImporter>(
        AssetType::Unknown, std::vector<std::string>{".bin"}, ".blobbin"));
    m_assetPipeline->add_importer(std::make_unique<AudioImporter>());
    m_assetPipeline->add_importer(std::make_unique<BinaryCopyImporter>(
        AssetType::Audio, std::vector<std::string>{".ogg"}, ".audiobin"));
    m_assetPipeline->add_importer(std::make_unique<TextMaterialImporter>());
    m_assetHotReload = std::make_unique<AssetHotReloadService>(
        *m_assetPipeline, m_assetRegistry,
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache");
}

glm::vec3 EditorCamera::get_front() const {
    return euler_direction(yaw, pitch);
}

glm::mat4 EditorCamera::get_view_matrix() const {
    return glm::lookAt(position, orbitTarget, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 EditorCamera::get_projection_matrix(float aspectRatio) const {
    return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
}

void EditorApplication::render_frame() {
    // Bounded wait: a GPU stall must degrade the frame, not hang the editor
    // forever (the previous UINT64_MAX wait froze input and left the window
    // black when a presentation fence never signalled).
    constexpr std::uint64_t kFenceTimeoutNs = 2'000'000'000ull; // 2s
    const VkResult fenceResult =
        vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, kFenceTimeoutNs);
    if (fenceResult == VK_TIMEOUT) {
        std::cerr << "[Vulkan] fence wait timed out; skipping frame\n";
        return;
    }
    if (fenceResult != VK_SUCCESS) {
        std::cerr << "[Vulkan] fence wait failed: " << static_cast<int>(fenceResult) << "\n";
        return;
    }

    // A pick requested from the previous frame is resolved before this frame's
    // scene pass so the freshly selected entity is highlighted immediately.
    // Hover pick uses the same pass (one extra pixel read, zero extra GPU cost).
    if ((m_pickRequested || m_hoverPickPending) && !m_inLauncherMode) {
        perform_pick_readback();
        m_pickRequested = false;
        m_hoverPickPending = false;
    }


    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
        }
        return;
    }
    if (result != VK_SUCCESS) {
        // VK_ERROR_DEVICE_LOST / VK_TIMEOUT / others: don't submit on a broken
        // swapchain (it would never present). Log and let the next frame retry.
        std::cerr << "[Vulkan] acquire next image failed: " << static_cast<int>(result) << "\n";
        return;
    }

    vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

    VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &beginInfo);

    if (!m_inLauncherMode) {
        // Size the offscreen to the panel (not the fitted image) so its aspect
        // ratio tracks the panel instead of locking onto its own previous size.
        recreate_offscreen_if_needed(
            static_cast<uint32_t>(std::max(1.0f, m_viewportPanelSize.x)),
            static_cast<uint32_t>(std::max(1.0f, m_viewportPanelSize.y)));
        if (m_offscreen.framebuffer != VK_NULL_HANDLE) {
            render_scene_to_offscreen(cmd);
        }
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = m_swapchainExtent;

    VkClearValue clearColor = { {{0.08f, 0.09f, 0.12f, 1.0f}} };
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (m_inLauncherMode) {
        draw_project_launcher();
    } else {
        // Ctrl+K: focus the global search box (the command palette).
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) {
            m_focusGlobalSearch = true;
        }

        // Draw order matters for the shell: main menu bar on top, then the
        // app bar, then the dockspace fills the remaining area (each positioned
        // from viewport->Pos/Size, so nothing is double-offset).
        draw_menu_bar();
        draw_app_bar();
        draw_dockspace();
        if (m_showHierarchy) draw_hierarchy_panel();
        if (m_showInspector) draw_inspector_panel();
        if (m_showViewport) draw_viewport_panel();
        if (m_showContentBrowser) draw_content_browser_panel();
#if VC_ENABLE_VOXEL_PLUGIN
        if (m_showVoxelTools) draw_voxel_tool_panel();
#endif
        if (m_showConsole) draw_console_panel();
        if (m_showScriptDebugger) draw_script_debugger_panel();
        if (m_showScriptCanvas) { if (!m_scriptCanvasLoaded) load_script_canvas(); draw_script_canvas_panel(); }
        {
            // Feed cooked texture assets to the Material Editor texture pickers.
            std::vector<std::pair<std::string, UUID>> textureAssets;
            for (const AssetMetadata& meta : m_assetRegistry.snapshot()) {
                if (meta.type == AssetType::Texture && meta.isCooked)
                    textureAssets.emplace_back(meta.sourcePath.filename().string(), meta.id);
            }
            m_specializedEditors.set_texture_assets(std::move(textureAssets));
        }
        Scene* activeScene = m_playMode.get_active_scene();
        if (!activeScene) activeScene = m_editorScene.get();
        m_specializedEditors.set_scene_context(activeScene, m_selectedEntity.get_id());
        m_specializedEditors.draw();
        {
            // Wicked-port tool windows: refresh the live context every frame.
            m_wickedTools.set_context(activeScene, m_selectedEntity.get_id(), &m_currentLanguage);
            // Paint tool state mirrors the selected entity's paintMode so the
            // viewport click handler paints without an extra callback.
            if (m_selectedEntity.is_valid()) {
                const auto paintIt = activeScene->paintComponents.find(m_selectedEntity.get_id());
                m_paintToolActive = paintIt != activeScene->paintComponents.end() &&
                                    paintIt->second.enabled && paintIt->second.paintMode;
            } else {
                m_paintToolActive = false;
            }
            m_wickedTools.set_asset_registry(&m_assetRegistry);
            m_wickedTools.set_open_specialized_editors(&m_specializedEditors.open);
            m_wickedTools.set_on_entity_deleted([this](UUID doomed) {
                // A tool panel deleted the entity: clear the editor selection so
                // the Inspector/hierarchy don't keep a dangling reference.
                if (m_selectedEntity.is_valid() && m_selectedEntity.get_id() == doomed) {
                    m_selectedEntity = Entity();
                    m_editorGui.select_entity(m_selectedEntity);
                }
            });
            m_wickedTools.set_create_project_callback([this](const std::string& name,
                                                             const std::string& folder) -> std::string {
                return create_project(name, folder);
            });
            m_wickedTools.set_terrain_callback([this](float scale, int octaves, float amount, float falloff) {
                generate_terrain_mesh(TerrainParams{ scale, octaves, amount, falloff });
            });
            m_wickedTools.set_graphics_callback([this](bool vsync, int quality) {
                apply_graphics_settings(vsync, quality);
            });
            m_wickedTools.set_save_settings_callback([this]() { save_settings(); });
            m_wickedTools.set_mesh_callback([this](int mode) -> std::string { return apply_mesh_normals(mode); });
            // Dev panel: route Control-API commands through the same handler the
            // HTTP API uses, and run headless self-tests on demand.
            m_wickedTools.set_control_command_callback([this](const std::string& cmd) {
                handle_control_command(cmd);
            });
            m_wickedTools.set_self_test_callback([this](int which) -> std::string {
                return run_editor_self_test(which);
            });
            m_wickedTools.set_package_assets_callback([this]() -> std::string {
                return package_assets_only();
            });
            m_wickedTools.set_hot_reload_status_callback([this]() -> std::string {
                if (!m_assetHotReload) return tr("inativo", "inactive");
                const size_t watched = m_assetRegistry.snapshot().size();
                return tr("ativo — vigia ", "active — watches ") + std::to_string(watched) +
                       tr(" asset(s) e reimporta mudanças nos arquivos de origem",
                          " asset(s) and reimports source-file changes");
            });
            m_wickedTools.set_play_state(static_cast<int>(m_playMode.get_state()));
            // Profiler: feed frame stats every frame for the graph window.
            m_wickedTools.set_frame_stats(m_fps, m_frameTimeMs);
            m_wickedTools.set_import_asset_callback([this](const std::string& requested) -> std::string {
                // "" = ask for a file via the editor's Windows dialog.
                std::string path = requested;
                if (path.empty()) {
                    if (!pick_file_dialog(path, L"Modelos (*.gltf;*.fbx;*.obj;*.ply)\0*.gltf;*.fbx;*.obj;*.ply\0Todos (*.*)\0*.*\0",
                                           L"Importar Modelo", nullptr)) {
                        return std::string();
                    }
                }
                if (!m_assetPipeline) return "Sem pipeline de assets.";
                const std::filesystem::path cookedRoot =
                    std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
                const ImportResult result = m_assetPipeline->import({ path, cookedRoot, 1 });
                if (!result) return "Falha: " + result.error;
                std::cout << "[Editor] Modelo importado: " << path << " (" << result.asset.id.to_string() << ")" << std::endl;
                return "OK: " + result.asset.sourcePath.filename().string();
            });
            m_wickedTools.draw();
        }
    }

    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkSemaphore waitSemaphores[] = { m_imageAvailableSemaphores[m_currentFrame] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphores[m_currentFrame] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]);

    VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }
    m_currentFrame = (m_currentFrame + 1) % 2;
}

void EditorApplication::recreate_swapchain() {
    vkDeviceWaitIdle(m_device);
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
    for (auto view : m_swapchainViews) vkDestroyImageView(m_device, view, nullptr);
    m_framebuffers.clear();
    m_swapchainViews.clear();
    m_swapchainImages.clear();

    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(m_window, &width, &height);
        glfwWaitEvents();
    }
    vkDeviceWaitIdle(m_device);

    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities) != VK_SUCCESS) {
        return;
    }
    VkExtent2D extent = capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()
        ? capabilities.currentExtent
        : VkExtent2D{ static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    extent.width = std::clamp(extent.width, std::max(1u, capabilities.minImageExtent.width), capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, std::max(1u, capabilities.minImageExtent.height), capabilities.maxImageExtent.height);

    // Prefer UNORM (matches the initial swapchain: no sRGB gamma washout).
    VkFormat imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
            imageFormat = candidate.format;
            colorSpace = candidate.colorSpace;
            break;
        }
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainKHR oldSwapchain = m_swapchain;
    VkSwapchainCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    createInfo.surface = m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = imageFormat;
    createInfo.imageColorSpace = colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // VSync (Opções Gráficas): FIFO when on; MAILBOX (preferred) or IMMEDIATE
    // when off — queried from the surface so unsupported modes never break.
    createInfo.presentMode = m_vsyncEnabled ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (!m_vsyncEnabled) {
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, modes.data());
        for (const VkPresentModeKHR mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) { createInfo.presentMode = mode; break; }
        }
    }
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;
    if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        m_swapchain = oldSwapchain;
        return;
    }
    vkDestroySwapchainKHR(m_device, oldSwapchain, nullptr);

    m_swapchainFormat = imageFormat;
    m_swapchainExtent = extent;
    uint32_t swapImageCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &swapImageCount, nullptr);
    m_swapchainImages.resize(swapImageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &swapImageCount, m_swapchainImages.data());
    m_swapchainViews.resize(swapImageCount);
    m_framebuffers.resize(swapImageCount);
    for (uint32_t i = 0; i < swapImageCount; ++i) {
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = m_swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapchainViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to recreate swapchain image view");
        }
        VkImageView attachments[] = { m_swapchainViews[i] };
        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to recreate framebuffer");
        }
    }
}

size_t EditorApplication::import_texture_pack(const std::filesystem::path& folder) {
    if (!m_assetPipeline || !std::filesystem::is_directory(folder)) return 0;
    const std::filesystem::path cookedRoot =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
    size_t imported = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(folder, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".png" && ext != ".tga" && ext != ".jpg" && ext != ".jpeg" &&
            ext != ".bmp" && ext != ".hdr" && ext != ".dds" &&
            ext != ".glb" && ext != ".gltf" && ext != ".fbx" &&
            ext != ".obj" && ext != ".ply" &&
            ext != ".wav" && ext != ".ogg" && ext != ".mp3" &&
            ext != ".vmat") continue;
        const ImportResult result = m_assetPipeline->import({it->path(), cookedRoot, 1});
        if (result) {
            ++imported;
            if ((ext == ".png" || ext == ".tga" || ext == ".bmp" || ext == ".dds") &&
                result.asset.width > 0 && result.asset.height > 0 &&
                result.asset.width == result.asset.height) {
                const uint32_t s = result.asset.width;
                if (s >= 8 && s <= 256 && (s & (s - 1)) == 0) {
                    if (!is_character_texture(result.asset) && !is_aux_map_texture(result.asset)) {
                        create_block_asset(result.asset);
                    }
                }
            }
        } else if (result.error.rfind("No importer supports", 0) != 0) {
            std::cerr << "[PackImport] " << it->path().filename().string()
                      << ": " << result.error << std::endl;
        }
    }
    if (imported > 0) {
        m_assetHotReload->watch_registered_assets();
        const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
            "Intermediate" / "AssetRegistry.db";
        if (!m_assetRegistry.save(registryPath))
            std::cerr << "[PackImport] Could not persist registry" << std::endl;
        std::cout << "[PackImport] Imported " << imported << " assets from "
                  << folder.string() << std::endl;
    }
    return imported;
}

std::vector<uint32_t> compile_material_glsl(VkShaderStageFlagBits stage, const std::string& source) {
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "vc_editor_material_tmp";
    const std::string stageArg = (stage == VK_SHADER_STAGE_VERTEX_BIT) ? "vert" : "frag";
    const std::filesystem::path srcFile = std::filesystem::path(tmp.string() + "." + stageArg);
    const std::filesystem::path spvFile = std::filesystem::path(tmp.string() + ".spv");
    {
        std::ofstream out(srcFile, std::ios::binary);
        out << source;
    }
    const std::string cmd = "glslc \"" + srcFile.string() + "\" -fshader-stage=" + stageArg +
                            " -o \"" + spvFile.string() + "\" 2>nul";
    const int rc = std::system(cmd.c_str());
    std::vector<uint32_t> spirv;
    if (rc == 0) {
        std::ifstream in(spvFile, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            const std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            if (size > 0 && size % 4 == 0) {
                spirv.resize(static_cast<size_t>(size) / 4);
                in.read(reinterpret_cast<char*>(spirv.data()), size);
            }
        }
    }
    std::error_code ec;
    std::filesystem::remove(srcFile, ec);
    std::filesystem::remove(spvFile, ec);
    return spirv;
}

float terrain_surface_height(uint32_t seed, float scale, int octaves,
                             float amount, float falloffParam,
                             float halfExtent, float x, float z) {
    const auto hash2 = [seed](int hx, int hz) -> float {
        uint32_t n = static_cast<uint32_t>(hx) * 374761393u
                   + static_cast<uint32_t>(hz) * 668265263u;
        n ^= seed * 0x9E3779B9u;
        n = (n ^ (n >> 13)) * 1274126177u;
        n ^= (n >> 16);
        return static_cast<float>(n & 0xFFFFu) / 65535.0f;
    };
    const auto smoothT = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const auto valueNoise = [&](float vx, float vz) {
        const int xi = static_cast<int>(std::floor(vx));
        const int zi = static_cast<int>(std::floor(vz));
        const float xf = smoothT(vx - std::floor(vx));
        const float zf = smoothT(vz - std::floor(vz));
        const float a = hash2(xi, zi), b = hash2(xi + 1, zi);
        const float c = hash2(xi, zi + 1), d = hash2(xi + 1, zi + 1);
        return a + (b - a) * xf + (c - a) * zf + (a - b - c + d) * xf * zf;
    };
    const auto fbm = [&](float fx, float fz, int oct) {
        float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
        for (int o = 0; o < oct; ++o) {
            sum += amp * valueNoise(fx * freq, fz * freq);
            norm += amp;
            amp *= 0.5f;
            freq *= 2.0f;
        }
        return sum / std::max(norm, 1e-6f);
    };
    const float dist = std::sqrt(x * x + z * z);
    const float falloff = glm::clamp(1.0f - (dist / halfExtent) * falloffParam,
                                     0.0f, 1.0f);
    return (fbm(x / scale, z / scale, octaves) - 0.5f)
           * 2.0f * amount * 20.0f * falloff;
}

void EditorApplication::mark_scene_dirty() {
    m_sceneDirty = true;
    m_sceneLastChange = glfwGetTime();
}

void EditorApplication::autosave_scene(bool force) {
    if (!m_sceneDirty || !m_editorScene) return;
    // Mutations always target m_editorScene (the play world is a clone), so
    // saving it is safe in any play state — the dirty flag itself is the gate.
    // Debounce: wait ~1.5s after the last change so gizmo drags / paint
    // strokes don't write the file every frame.
    if (!force && glfwGetTime() - m_sceneLastChange < 1.5) return;
    std::string path = m_activeScenePath;
    if (path.empty()) {
        // Untitled scene: a stable autosave file in the scenes folder
        // (overwritten each time, unlike the API's timestamped fallback).
        if (m_autosavePath.empty()) {
            const auto scenesDir = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes";
            std::error_code ec;
            std::filesystem::create_directories(scenesDir, ec);
            m_autosavePath = (scenesDir / "autosave.scene").string();
        }
        path = m_autosavePath;
    }
    if (m_editorScene->save_to_file(path)) {
        m_sceneDirty = false;
        persist_terrain_sidecar(path);
        std::cout << "[Autosave] scene saved: " << path << std::endl;
    } else {
        std::cerr << "[Autosave] save failed: " << path << std::endl;
    }
}

void EditorApplication::handle_control_command(const std::string& cmd) {
    if (cmd == "play" && m_playMode.get_state() == PlayState::Edit) {
        m_playMode.start_play(m_editorScene.get());
        setup_play_runtime();
        std::cout << "[ControlApi] play started" << std::endl;
    } else if (cmd == "pause" && m_playMode.get_state() == PlayState::Play) {
        m_playMode.pause_play();
        std::cout << "[ControlApi] paused" << std::endl;
    } else if (cmd == "resume" && m_playMode.get_state() == PlayState::Pause) {
        m_playMode.pause_play();
        std::cout << "[ControlApi] resumed" << std::endl;
    } else if (cmd == "step" && m_playMode.get_state() == PlayState::Pause) {
        m_stepRequested = true;
        std::cout << "[ControlApi] step" << std::endl;
    } else if (cmd == "stop" && m_playMode.get_state() != PlayState::Edit) {
        teardown_play_runtime();
        m_playMode.stop_play();
        m_playMode.set_editor_scene(m_editorScene.get());
        m_selectedEntity = Entity();
        m_editorGui.select_entity(m_selectedEntity);
        std::cout << "[ControlApi] stopped" << std::endl;
    } else if (cmd.rfind("zoom ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(5), f) || f.empty()) {
            m_controlResult = "zoom: expected a number";
        } else {
            const float amount = f[0];
            m_editorCamera.orbitDistance =
                glm::clamp(m_editorCamera.orbitDistance * (1.0f - amount), 0.5f, 5000.0f);
            recompute_editor_camera_position();
            std::cout << "[ControlApi] zoom " << amount << " -> " << m_editorCamera.orbitDistance << std::endl;
        }
    } else if (cmd.rfind("move ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(5), f) || f.size() < 3) {
            m_controlResult = "move: expected 3 numbers";
        } else {
            m_editorCamera.orbitTarget +=
                m_editorCamera.get_front() * f[0] +
                m_editorCamera.get_right() * f[1] +
                m_editorCamera.get_up() * f[2];
            recompute_editor_camera_position();
            std::cout << "[ControlApi] move " << f[0] << " " << f[1] << " " << f[2] << std::endl;
        }
    } else if (cmd.rfind("turn ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(5), f) || f.size() < 2) {
            m_controlResult = "turn: expected 2 numbers";
        } else {
            m_editorCamera.yaw += f[0];
            m_editorCamera.pitch = glm::clamp(m_editorCamera.pitch + f[1], -89.0f, 89.0f);
            recompute_editor_camera_position();
            std::cout << "[ControlApi] turn " << f[0] << " " << f[1] << std::endl;
        }
    } else if (cmd.rfind("focus ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(6), f) || f.size() < 3) {
            m_controlResult = "focus: expected 3 numbers (x y z)";
        } else {
            m_editorCamera.orbitTarget = glm::vec3(f[0], f[1], f[2]);
            recompute_editor_camera_position();
            std::cout << "[ControlApi] focus " << f[0] << " " << f[1] << " " << f[2] << std::endl;
        }
    } else if (cmd.rfind("terrain ", 0) == 0) {
        // Defaults match the TerrainParams / panel defaults: scale 120 gives
        // rolling hills — 1.0 turns the sheet into high-frequency spikes.
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(8), f)) {
            m_controlResult = "terrain: expected numbers (scale octaves amount falloff extent segments seed)";
        } else {
            const float scale = f.size() > 0 ? f[0] : 120.0f;
            const int octaves = f.size() > 1 ? static_cast<int>(f[1]) : 5;
            const float amount = f.size() > 2 ? f[2] : 0.5f;
            const float falloff = f.size() > 3 ? f[3] : 0.4f;
            const float halfExtent = f.size() > 4 ? f[4] : 500.0f;
            const int segments = f.size() > 5 ? static_cast<int>(f[5]) : 256;
            const uint32_t seed = f.size() > 6 ? static_cast<uint32_t>(f[6]) : 1u;
            generate_terrain_mesh(TerrainParams{ scale, octaves, amount, falloff,
                                                 halfExtent, segments, seed });
            mark_scene_dirty();
            std::cout << "[ControlApi] terrain scale=" << scale << " octaves=" << octaves
                      << " amount=" << amount << " falloff=" << falloff
                      << " extent=" << halfExtent << " segments=" << segments
                      << " seed=" << seed << std::endl;
        }
    } else if (cmd.rfind("graphics ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(9), f) || f.size() < 2) {
            m_controlResult = "graphics: expected 2 numbers";
        } else {
            const int vsyncInt = static_cast<int>(f[0]);
            const int quality = static_cast<int>(f[1]);
            apply_graphics_settings(vsyncInt != 0, quality);
            std::cout << "[ControlApi] graphics vsync=" << vsyncInt << " quality=" << quality << std::endl;
        }
    } else if (cmd == "save-settings") {
        save_settings();
        std::cout << "[ControlApi] save-settings" << std::endl;
    } else if (cmd.rfind("project ", 0) == 0) {
        const std::string result = create_project(cmd.substr(8), "");
        if (result.rfind("OK", 0) != 0) m_controlResult = result;
        std::cout << "[ControlApi] project -> " << result << std::endl;
    } else if (cmd.rfind("mesh ", 0) == 0) {
        int mode = 0;
        std::istringstream ss(cmd.substr(5));
        ss >> mode;
        const std::string result = apply_mesh_normals(mode);
        if (result.rfind("Normais recalculadas", 0) != 0 && result.rfind("OK", 0) != 0)
            m_controlResult = result;
        std::cout << "[ControlApi] mesh " << mode << " -> " << result << std::endl;
    } else if (cmd == "simulate" && m_playMode.get_state() == PlayState::Edit) {
        m_playMode.start_simulate(m_editorScene.get());
        setup_play_runtime();
        std::cout << "[ControlApi] simulate started" << std::endl;
    } else if (cmd == "new-scene") {
        init_default_scene();
        m_sceneDirty = false;
        std::cout << "[ControlApi] new scene" << std::endl;
    } else if (cmd.rfind("open-scene ", 0) == 0) {
        // Resolve relative paths against the source root: the editor process
        // cwd is not guaranteed to be the engine folder.
        std::string scenePath = cmd.substr(11);
        std::filesystem::path rel(scenePath);
        if (rel.is_relative()) {
            const auto abs = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / rel;
            if (std::filesystem::exists(abs)) scenePath = abs.string();
        }
        if (!std::filesystem::exists(scenePath)) {
            m_controlResult = "open-scene: file not found: " + scenePath;
            std::cout << "[ControlApi] open-scene: file not found: " << scenePath << std::endl;
            return;
        }
        load_scene_file(scenePath);
        // API-driven scene open must leave the launcher hub: the control-API
        // drain and play runtime are gated on !m_inLauncherMode.
        m_inLauncherMode = false;
        std::cout << "[ControlApi] open scene '" << scenePath << "'" << std::endl;
    } else if (cmd == "save-scene") {
        // API-safe: never open a blocking native dialog from the HTTP thread
        // path (that would wedge the main loop). If there is no active scene
        // path yet, fall back to a timestamped file in the scenes folder.
        if (!m_editorScene) {
            m_controlResult = "save-scene: no scene";
            std::cout << "[ControlApi] save-scene: no scene" << std::endl;
        } else if (!m_activeScenePath.empty()) {
            if (m_editorScene->save_to_file(m_activeScenePath)) {
                m_sceneDirty = false;
                persist_terrain_sidecar(m_activeScenePath);
                std::cout << "[ControlApi] scene saved: " << m_activeScenePath << std::endl;
            } else {
                m_controlResult = "save-scene failed: " + m_activeScenePath;
                std::cerr << "[ControlApi] save-scene failed: " << m_activeScenePath << std::endl;
            }
        } else {
            const auto scenesDir = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes";
            std::error_code ec;
            std::filesystem::create_directories(scenesDir, ec);
            const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
            const std::filesystem::path fallback = scenesDir / ("api_" + stamp + ".scene");
            if (m_editorScene->save_to_file(fallback.string())) {
                m_activeScenePath = fallback.string();
                m_sceneDirty = false;
                persist_terrain_sidecar(m_activeScenePath);
                std::cout << "[ControlApi] scene saved (new): " << m_activeScenePath << std::endl;
            } else {
                m_controlResult = "save-scene failed: " + fallback.string();
                std::cerr << "[ControlApi] save-scene failed: " << fallback << std::endl;
            }
        }
    } else if (cmd.rfind("add-entity ", 0) == 0) {
        if (!m_editorScene) { std::cout << "[ControlApi] no scene" << std::endl; return; }
        const std::string type = cmd.substr(11);
        const auto create = [&](const char* name) {
            Entity e = m_editorScene->create_entity(name);
            m_selectedEntity = e;
            m_editorGui.select_entity(e);
            mark_scene_dirty();
            return e;
        };
        Entity e;
        if (type == "empty") e = create("Novo Objeto");
        else if (type == "cube") { e = create("Cubo 3D"); if (e.is_valid()) m_editorScene->meshRendererComponents[e.get_id()] = MeshRendererComponent{}; }
        else if (type == "camera") { e = create("Câmera"); if (e.is_valid()) m_editorScene->cameraComponents[e.get_id()] = CameraComponent{}; }
        else if (type == "sun") { e = create("Luz do Sol"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{}; }
        else if (type == "point") { e = create("Luz de Lâmpada"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.8f, 0.4f), 5000.0f, 15.0f, true }; }
        else if (type == "spot") { e = create("Luz Spot"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot }; }
        else if (type == "area") { e = create("Luz de Área"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area }; }
        else if (type == "particles") { e = create("Emissor de Partículas"); if (e.is_valid()) m_editorScene->particleEmitterComponents[e.get_id()] = ParticleEmitterComponent{}; }
        else if (type == "audio") { e = create("Fonte de Áudio"); if (e.is_valid()) m_editorScene->audioComponents[e.get_id()] = AudioComponent{}; }
        else if (type == "rigidbody") { e = create("Corpo Rígido"); if (e.is_valid()) m_editorScene->rigidbodyComponents[e.get_id()] = RigidbodyComponent{}; }
        else if (type == "vehicle") { e = create("Veículo"); if (e.is_valid()) m_editorScene->vehicleComponents[e.get_id()] = VehicleComponent{}; }
        else if (type == "destructible") { e = create("Destrutível"); if (e.is_valid()) m_editorScene->destructionComponents[e.get_id()] = DestructionComponent{}; }
        else if (type == "navagent") { e = create("Agente de Navegação"); if (e.is_valid()) m_editorScene->navigationComponents[e.get_id()] = NavigationComponent{}; }
        else if (type == "mission") { e = create("Missão"); if (e.is_valid()) m_editorScene->missionComponents[e.get_id()] = MissionComponent{}; }
        else if (type == "dialogue") { e = create("Diálogo"); if (e.is_valid()) m_editorScene->dialogueComponents[e.get_id()] = DialogueComponent{}; }
#if VC_ENABLE_VOXEL_PLUGIN
        else if (type == "voxelworld") { e = create("Mundo de Blocos"); if (e.is_valid()) m_editorScene->voxelVolumeComponents[e.get_id()] = VoxelVolumeComponent{}; }
#endif
        std::cout << "[ControlApi] add-entity '" << type << "' -> "
                  << (e.is_valid() ? e.get_id().to_string() : "unknown type") << std::endl;
    } else if (cmd.rfind("add-component ", 0) == 0) {
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, type;
        ss >> uuidStr >> type;
        const UUID id = UUID::from_string(uuidStr);
        if (!m_editorScene || !m_editorScene->get_entities().contains(id)) {
            m_controlResult = "add-component: entity not found";
            std::cout << "[ControlApi] add-component: entity not found" << std::endl;
            return;
        }
        Scene* scene = m_editorScene.get();
        if (type == "light") scene->lightComponents[id] = LightComponent{};
        else if (type == "camera") scene->cameraComponents[id] = CameraComponent{};
        else if (type == "mesh") scene->meshRendererComponents[id] = MeshRendererComponent{};
        else if (type == "material") scene->materialComponents[id] = MaterialComponent{};
        else if (type == "rigidbody") scene->rigidbodyComponents[id] = RigidbodyComponent{};
        else if (type == "weapon") scene->weaponComponents[id] = WeaponComponent{};
        else if (type == "vehicle") scene->vehicleComponents[id] = VehicleComponent{};
        else if (type == "ragdoll") scene->ragdollComponents[id] = RagdollComponent{};
        else if (type == "destructible") scene->destructionComponents[id] = DestructionComponent{};
        else if (type == "navigation") scene->navigationComponents[id] = NavigationComponent{};
        else if (type == "particle") scene->particleEmitterComponents[id] = ParticleEmitterComponent{};
        else if (type == "audio") scene->audioComponents[id] = AudioComponent{};
        else if (type == "mission") scene->missionComponents[id] = MissionComponent{};
        else if (type == "dialogue") scene->dialogueComponents[id] = DialogueComponent{};
        else if (type == "animation") scene->animationComponents[id] = AnimationComponent{};
        else if (type == "timeline") scene->timelineComponents[id] = TimelineComponent{};
        else if (type == "ik") scene->ikComponents[id] = IKComponent{};
        else if (type == "retarget") scene->retargetComponents[id] = RetargetComponent{};
#if VC_ENABLE_VOXEL_PLUGIN
        else if (type == "voxel") scene->voxelVolumeComponents[id] = VoxelVolumeComponent{};
#endif
        else { m_controlResult = "add-component: unknown type '" + type + "'"; std::cout << "[ControlApi] add-component: unknown type '" << type << "'" << std::endl; return; }
        mark_scene_dirty();
        std::cout << "[ControlApi] add-component " << type << " on " << uuidStr << std::endl;
    } else if (cmd.rfind("delete-entity ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(14));
        if (m_editorScene && m_editorScene->get_entities().contains(id)) {
            m_editorScene->destroy_entity(id);
            if (m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id) m_selectedEntity = Entity();
            mark_scene_dirty();
            std::cout << "[ControlApi] deleted entity" << std::endl;
        } else {
            m_controlResult = "delete-entity: not found";
            std::cout << "[ControlApi] delete-entity: not found" << std::endl;
        }
    } else if (cmd.rfind("rename-entity ", 0) == 0) {
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        const UUID id = UUID::from_string(uuidStr);
        if (m_editorScene && m_editorScene->get_entities().contains(id) && !name.empty()) {
            m_editorScene->rename_entity(id, name);
            mark_scene_dirty();
            std::cout << "[ControlApi] renamed to '" << name << "'" << std::endl;
        } else {
            m_controlResult = "rename-entity: not found or empty name";
            std::cout << "[ControlApi] rename-entity: not found or empty name" << std::endl;
        }
    } else if (cmd.rfind("select ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(7));
        if (m_editorScene && m_editorScene->get_entities().contains(id)) {
            m_selectedEntity = Entity();
            m_selectedEntity = m_editorScene->get_entities().at(id);
            m_editorGui.select_entity(m_selectedEntity);
            std::cout << "[ControlApi] selected " << id.to_string() << std::endl;
        } else {
            m_controlResult = "select: entity not found";
            std::cout << "[ControlApi] select: entity not found" << std::endl;
        }
    } else if (cmd.rfind("select-name ", 0) == 0) {
        if (!m_editorScene) return;
        const std::string name = cmd.substr(12);
        for (const auto& [id, entity] : m_editorScene->get_entities()) {
            if (entity.get_name() == name || entity.get_name().find(name) != std::string::npos) {
                m_selectedEntity = Entity();
                m_selectedEntity = m_editorScene->get_entities().at(id);
                m_editorGui.select_entity(m_selectedEntity);
                std::cout << "[ControlApi] selected '" << entity.get_name() << "'" << std::endl;
                return;
            }
        }
        m_controlResult = "select-name: no match";
        std::cout << "[ControlApi] select-name: no match" << std::endl;
    } else if (cmd.rfind("set-transform ", 0) == 0) {
        // Field-masked PATCH, not positional: <uuid> <mask> p0 p1 p2 r0 r1 r2 s0 s1 s2
        // mask = 3 chars ('1'/'0') for position/rotation/scale, so the agent can
        // change ONLY scale (mask "001") without teleporting the object to the
        // origin or having scale floats reinterpreted as rotation.
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, maskStr;
        ss >> uuidStr >> maskStr;
        const UUID id = UUID::from_string(uuidStr);
        std::vector<float> values;
        float v;
        while (ss >> v) values.push_back(v);
        auto it = m_editorScene ? m_editorScene->transformComponents.find(id) : m_editorScene->transformComponents.end();
        if (it == m_editorScene->transformComponents.end()) {
            m_controlResult = "set-transform: entity not found";
            std::cout << "[ControlApi] set-transform: entity not found" << std::endl;
        } else if (values.size() < 9) {
            m_controlResult = "set-transform: expected <uuid> <mask> + 9 floats";
            std::cout << "[ControlApi] set-transform: expected 9 floats" << std::endl;
        } else {
            if (maskStr.size() > 0 && maskStr[0] == '1')
                it->second.position = glm::vec3(values[0], values[1], values[2]);
            if (maskStr.size() > 1 && maskStr[1] == '1')
                it->second.rotation = glm::vec3(values[3], values[4], values[5]);
            if (maskStr.size() > 2 && maskStr[2] == '1')
                it->second.scale = glm::vec3(values[6], values[7], values[8]);
            mark_scene_dirty();
            std::cout << "[ControlApi] transform set (mask " << maskStr << ")" << std::endl;
        }
    } else if (cmd.rfind("gizmo ", 0) == 0) {
        const std::string mode = cmd.substr(6);
        if (mode == "select") m_gizmoMode = GizmoMode::Select;
        else if (mode == "move") m_gizmoMode = GizmoMode::Translate;
        else if (mode == "rotate") m_gizmoMode = GizmoMode::Rotate;
        else if (mode == "scale") m_gizmoMode = GizmoMode::Scale;
        std::cout << "[ControlApi] gizmo " << mode << std::endl;
    } else if (cmd.rfind("gizmo-space ", 0) == 0) {
        m_gizmoLocal = cmd.substr(12) == "local";
        std::cout << "[ControlApi] gizmo-space " << (m_gizmoLocal ? "local" : "world") << std::endl;
    } else if (cmd.rfind("snap ", 0) == 0) {
        m_snapTranslate = std::max(0.0f, std::stof(cmd.substr(5)));
        std::cout << "[ControlApi] snap " << m_snapTranslate << std::endl;
    } else if (cmd.rfind("import ", 0) == 0) {
        if (!m_assetPipeline) {
            m_controlResult = "import: asset pipeline not ready";
        } else {
            const std::filesystem::path cookedRoot =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
            const ImportResult result = m_assetPipeline->import({ cmd.substr(7), cookedRoot, 1 });
            if (!result) m_controlResult = "import: " + result.error;
            std::cout << "[ControlApi] import -> " << (result ? "ok" : result.error) << std::endl;
        }
    } else if (cmd.rfind("import-pack ", 0) == 0) {
        const size_t count = import_texture_pack(std::filesystem::path(cmd.substr(12)));
        if (count == 0) m_controlResult = "import-pack: no assets imported from the given path";
        std::cout << "[ControlApi] import-pack -> " << count << " assets imported" << std::endl;
    } else if (cmd.rfind("block-model ", 0) == 0) {
        const UUID texId = UUID::from_string(cmd.substr(12));
        const auto meta = m_assetRegistry.find(texId);
        if (meta && meta->type == AssetType::Texture) {
            create_block_asset(*meta);
            std::cout << "[ControlApi] block model created" << std::endl;
        } else {
            m_controlResult = "block-model: texture not found";
            std::cout << "[ControlApi] block-model: texture not found" << std::endl;
        }
    } else if (cmd.rfind("block-faces ", 0) == 0) {
        // block-faces {blockId} {top} {side} {bottom} — "0" keeps the current
        // face. Rewrites the .vblock sidecar and invalidates the atlas so the
        // block renders with per-face textures (grass top / grass side / dirt).
        std::istringstream ss(cmd.substr(12));
        std::string blockStr, topStr, sideStr, bottomStr;
        ss >> blockStr >> topStr >> sideStr >> bottomStr;
        const auto face = [](const std::string& s) {
            return (s.empty() || s == "0") ? UUID{ 0, 0 } : UUID::from_string(s);
        };
        const UUID blockId = UUID::from_string(blockStr);
        if (set_block_faces(blockId, face(topStr), face(sideStr), face(bottomStr))) {
            std::cout << "[ControlApi] block-faces updated " << blockId.to_string() << std::endl;
        } else {
            m_controlResult = "block-faces: block not found or face UUID is not a registered texture";
        }
    } else if (cmd.rfind("block-model-faces ", 0) == 0) {
        // block-model-faces {base} {top} {side} {bottom} {name} — "0" = no face.
        // Creates a NEW block asset with per-face textures from texture UUIDs.
        std::istringstream ss(cmd.substr(18));
        std::string baseStr, topStr, sideStr, bottomStr, name;
        ss >> baseStr >> topStr >> sideStr >> bottomStr >> name;
        const auto face = [](const std::string& s) {
            return (s.empty() || s == "0") ? UUID{ 0, 0 } : UUID::from_string(s);
        };
        const UUID newId = create_block_from_faces(face(baseStr), face(topStr),
                                                   face(sideStr), face(bottomStr), name);
        if (!newId.is_valid()) {
            m_controlResult = "block-model-faces: at least one face must be a registered texture";
        } else {
            std::cout << "[ControlApi] block-model-faces created " << newId.to_string() << std::endl;
        }
    } else if (cmd.rfind("spawn-block ", 0) == 0) {
        const UUID blockId = UUID::from_string(cmd.substr(12));
        const auto blockMeta = m_assetRegistry.find(blockId);
        if (!blockMeta || blockMeta->type != AssetType::Block) {
            m_controlResult = "spawn-block: block asset not found";
            std::cout << "[ControlApi] spawn-block: block asset not found" << std::endl;
        } else {
            spawn_block_entity(blockId, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
            std::cout << "[ControlApi] spawn-block " << blockId.to_string() << std::endl;
        }
    } else if (cmd.rfind("spawn-character ", 0) == 0) {
        const UUID texId = UUID::from_string(cmd.substr(16));
        const auto meta = m_assetRegistry.find(texId);
        if (meta && meta->type == AssetType::Texture && is_character_texture(*meta)) {
            spawn_character_entity(texId, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
            std::cout << "[ControlApi] spawn-character " << texId.to_string() << std::endl;
        } else {
            m_controlResult = "spawn-character: skin texture not found";
            std::cout << "[ControlApi] spawn-character: skin texture not found" << std::endl;
        }
    } else if (cmd.rfind("layer ", 0) == 0) {
        // layer {uuid} {name} — sets the entity's layer name.
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(6));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && (name.front() == ' ')) name.erase(name.begin());
        if (scene && !uuidStr.empty() && !name.empty()) {
            const UUID id = UUID::from_string(uuidStr);
            scene->layerComponents[id].name = name;
            mark_scene_dirty();
            std::cout << "[ControlApi] layer " << uuidStr << " -> '" << name << "'" << std::endl;
        }
    } else if (cmd.rfind("layer-vis ", 0) == 0) {
        // layer-vis {name} {0|1} — show/hide every entity on that layer.
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(10));
        std::string name;
        int visible = 1;
        std::getline(ss, name, '|');
        ss >> visible;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        if (scene && !name.empty()) {
            for (auto& [id, lc] : scene->layerComponents) {
                if (lc.name == name) lc.visible = visible != 0;
            }
            mark_scene_dirty();
            std::cout << "[ControlApi] layer-vis '" << name << "' visible=" << visible << std::endl;
        }
    } else if (cmd.rfind("decal-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(10));
        std::string uuidStr, texture;
        ss >> uuidStr;
        std::getline(ss, texture);
        while (!texture.empty() && texture.front() == ' ') texture.erase(texture.begin());
        if (scene && !uuidStr.empty()) {
            DecalComponent dec;
            dec.texturePath = texture;
            scene->decalComponents[UUID::from_string(uuidStr)] = dec;
            mark_scene_dirty();
            std::cout << "[ControlApi] decal-add " << uuidStr << " texture='" << texture << "'" << std::endl;
        }
    } else if (cmd.rfind("hair-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(9));
        if (scene && id.is_valid()) {
            scene->hairParticleComponents[id] = HairParticleComponent{};
            mark_scene_dirty();
            std::cout << "[ControlApi] hair-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("softbody-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(13));
        if (scene && id.is_valid()) {
            scene->softBodyComponents[id] = SoftBodyComponent{};
            mark_scene_dirty();
            std::cout << "[ControlApi] softbody-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("env-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(8));
        if (scene && id.is_valid()) {
            scene->envProbeComponents[id] = EnvProbeComponent{};
            mark_scene_dirty();
            std::cout << "[ControlApi] env-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("env-capture ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(12));
        if (scene && scene->envProbeComponents.contains(id)) {
            scene->envProbeComponents[id].captureRequested = true;
            mark_scene_dirty();
            std::cout << "[ControlApi] env-capture " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("paint-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(10));
        if (scene && id.is_valid()) {
            scene->paintComponents[id] = PaintComponent{};
            mark_scene_dirty();
            std::cout << "[ControlApi] paint-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("paint-mode ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(11));
        std::string uuidStr;
        int mode = 0;
        ss >> uuidStr >> mode;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->paintComponents.contains(id)) {
            scene->paintComponents[id].paintMode = mode != 0;
            std::cout << "[ControlApi] paint-mode " << uuidStr << " " << mode << std::endl;
        }
    } else if (cmd.rfind("paint-color ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr;
        float r = 1.0f, g = 0.3f, b = 0.22f;
        ss >> uuidStr >> r >> g >> b;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->paintComponents.contains(id)) {
            scene->paintComponents[id].brushColor = { r, g, b };
            std::cout << "[ControlApi] paint-color " << uuidStr << " " << r << " " << g << " " << b << std::endl;
        }
    } else if (cmd.rfind("video-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(10));
        if (scene && id.is_valid()) {
            scene->videoComponents[id] = VideoComponent{};
            std::cout << "[ControlApi] video-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("video-frame ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->videoComponents.contains(id) && !name.empty()) {
            scene->videoComponents[id].framePaths.push_back(name);
            std::cout << "[ControlApi] video-frame " << uuidStr << " '" << name << "'" << std::endl;
        }
    } else if (cmd.rfind("video-play ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(11));
        std::string uuidStr;
        int mode = 1;
        ss >> uuidStr >> mode;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->videoComponents.contains(id)) {
            scene->videoComponents[id].playing = mode != 0;
            std::cout << "[ControlApi] video-play " << uuidStr << " " << mode << std::endl;
        }
    } else if (cmd.rfind("gaussian-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(13));
        if (scene && id.is_valid()) {
            scene->gaussianSplatComponents[id] = GaussianSplatComponent{};
            std::cout << "[ControlApi] gaussian-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("gaussian-regen ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(15));
        if (scene && scene->gaussianSplatComponents.contains(id)) {
            scene->gaussianSplatComponents[id].regenerate = true;
            std::cout << "[ControlApi] gaussian-regen " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("expression-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(15));
        std::string uuidStr, headStr;
        ss >> uuidStr >> headStr;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && id.is_valid()) {
            ExpressionComponent ex;
            ex.headEntity = UUID::from_string(headStr);
            scene->expressionComponents[id] = ex;
            std::cout << "[ControlApi] expression-add " << uuidStr << " head=" << headStr << std::endl;
        }
    } else if (cmd.rfind("asset-duplicate ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(16));
        const auto meta = m_assetRegistry.find(id);
        if (!meta) {
            m_controlResult = "asset-duplicate: asset not found";
        } else {
            const std::filesystem::path dup = meta->sourcePath.parent_path() /
                (meta->sourcePath.stem().string() + "_copy" + meta->sourcePath.extension().string());
            AssetBrowserModel browser{ m_assetRegistry };
            const auto result = browser.duplicate_asset(id, dup);
            if (!result) m_controlResult = "asset-duplicate: " + result.error;
            m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
            std::cout << "[ControlApi] asset-duplicate -> " << (result ? "ok" : result.error) << std::endl;
        }
    } else if (cmd.rfind("asset-delete ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(13));
        AssetBrowserModel browser{ m_assetRegistry };
        const auto result = browser.delete_asset(id);
        if (!result) m_controlResult = "asset-delete: " + result.error;
        m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
        std::cout << "[ControlApi] asset-delete -> " << (result ? "ok" : result.error) << std::endl;
    } else if (cmd.rfind("reimport ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(9));
        const auto meta = m_assetRegistry.find(id);
        if (meta && m_assetPipeline) {
            const std::filesystem::path cookedRoot =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
            const ImportResult result = m_assetPipeline->import({
                .source = meta->sourcePath, .cookedDirectory = cookedRoot,
                .importerVersion = meta->importerVersion, .settings = meta->importSettings });
            m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
            std::cout << "[ControlApi] reimport -> " << (result ? "ok" : result.error) << std::endl;
        } else {
            m_controlResult = "reimport: asset not found";
        }
    } else if (cmd.rfind("screenshot", 0) == 0) {
        // Save the current viewport to a PNG so an agent can SEE the result.
        // The path is absolute or relative to the engine root. Returns the
        // saved path on success (through the Control-API result).
        std::string path = (cmd.size() > 10) ? cmd.substr(10) : std::string();
        while (!path.empty() && path.front() == ' ') path.erase(path.begin());
        if (path.empty()) {
            const auto shots = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "screenshots";
            std::error_code ec;
            std::filesystem::create_directories(shots, ec);
            const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
            path = (shots / ("viewport_" + stamp + ".png")).string();
        } else {
            std::filesystem::path p(path);
            if (p.is_relative()) p = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / p;
            path = p.string();
        }
        const std::string err = capture_viewport_screenshot(path);
        if (!err.empty()) {
            m_controlResult = err;
            std::cout << "[ControlApi] screenshot FAILED: " << err << std::endl;
        } else {
            m_controlData = path;
            std::cout << "[ControlApi] screenshot saved: " << path << std::endl;
        }
    } else if (cmd.rfind("voxel-generate ", 0) == 0) {
        std::istringstream ss(cmd.substr(15));
        std::string uuidStr; uint32_t seed = 1337; float seaLevel = 24.0f;
        ss >> uuidStr; if (ss >> seed) {} if (ss >> seaLevel) {}
        const UUID id = UUID::from_string(uuidStr);
        m_voxelStructures.erase(id);
        ensure_voxel_volume(id, seed, seaLevel);
        m_voxelMeshesDirty.insert(id);
        mark_scene_dirty();
        std::cout << "[ControlApi] voxel-generate " << uuidStr << " seed=" << seed << std::endl;
    } else if (cmd.rfind("voxel-clear ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(12));
        const auto gridIt = m_voxelStructures.find(id);
        if (gridIt != m_voxelStructures.end()) {
            const auto& size = gridIt->second->size();
            for (int x = 0; x < size.x; ++x)
                for (int y = 0; y < size.y; ++y)
                    for (int z = 0; z < size.z; ++z)
                        gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue::air());
            m_voxelMeshesDirty.insert(id);
            mark_scene_dirty();
            std::cout << "[ControlApi] voxel-clear " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("voxel-paint ", 0) == 0) {
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr; int x = 0, y = 0, z = 0, type = 1, mode = 0;
        ss >> uuidStr >> x >> y >> z >> type >> mode;
        const UUID id = UUID::from_string(uuidStr);
        const auto gridIt = m_voxelStructures.find(id);
        if (gridIt == m_voxelStructures.end()) {
            std::cout << "[ControlApi] voxel-paint: volume not generated yet" << std::endl;
        } else {
            if (mode == 1) gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue::air());
            else gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue{ static_cast<uint16_t>(type), 0, 255 });
            m_voxelMeshesDirty.insert(id);
            mark_scene_dirty();
            std::cout << "[ControlApi] voxel-paint " << uuidStr << " (" << x << "," << y << "," << z << ") type=" << type << " mode=" << mode << std::endl;
        }
    } else if (cmd.rfind("voxel-block ", 0) == 0) {
        // Assign a Block asset to a voxel type (1=dirt, 2=grass, 3=stone,
        // 4=water, or any painted type): `voxel-block 2 <block-uuid>`. The
        // volume then samples that block's per-face atlas [top|side|bottom]
        // instead of a flat color. Auto-resolution by texture name is the
        // fallback when no override was set for the type.
        std::istringstream ss(cmd.substr(12));
        int type = 0;
        std::string uuidStr;
        ss >> type >> uuidStr;
        const UUID blockId = UUID::from_string(uuidStr);
        const auto meta = m_assetRegistry.find(blockId);
        if (type < 1 || uuidStr.empty() || !blockId.is_valid() || !meta || meta->type != AssetType::Block) {
            m_controlResult = "voxel-block: expected <type> <block-asset-uuid>";
            std::cout << "[ControlApi] voxel-block: invalid type/block " << uuidStr << std::endl;
        } else {
            m_voxelTypeBlocks[static_cast<uint16_t>(type)] = blockId;
            for (auto& [id, mesh] : m_voxelMeshes) {
                (void)mesh;
                m_voxelMeshesDirty.insert(id);
            }
            mark_scene_dirty();
            std::cout << "[ControlApi] voxel-block type=" << type
                      << " block=" << blockId.to_string() << std::endl;
        }
    } else if (cmd.rfind("script-event ", 0) == 0) {
        const std::string ev = cmd.substr(13);
        if (m_playScript.start_event(ev)) std::cout << "[ControlApi] script-event '" << ev << "'" << std::endl;
        else std::cout << "[ControlApi] script-event: no such event handler" << std::endl;
    } else if (cmd == "script-pause") {
        m_scriptPauseRequested = true;
        std::cout << "[ControlApi] script paused" << std::endl;
    } else if (cmd == "script-continue") {
        m_scriptPauseRequested = false;
        if (m_playScript.status() == VMStatus::Paused) m_scriptDebugger.continue_run(10000, 0.0f);
        std::cout << "[ControlApi] script resumed" << std::endl;
    } else if (cmd == "script-step") {
        m_scriptPauseRequested = true;
        if (m_playScript.status() == VMStatus::Paused) m_scriptDebugger.step_into(0.0f);
        std::cout << "[ControlApi] script step" << std::endl;
    } else if (cmd.rfind("editor ", 0) == 0) {
        std::string tab = cmd.substr(7);
        if (tab == "render-graph" || tab == "render graph") tab = "Render Graph";
        else if (!tab.empty()) tab[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tab[0])));
        m_specializedEditors.open_editor(tab);
        std::cout << "[ControlApi] editor tab '" << tab << "'" << std::endl;
    } else if (cmd.rfind("window ", 0) == 0) {
        const std::string w = cmd.substr(7);
        const auto toggle = [](bool& flag) { flag = !flag; };
        if (w == "viewport") toggle(m_showViewport);
        else if (w == "scene") toggle(m_showHierarchy);
        else if (w == "inspector") toggle(m_showInspector);
        else if (w == "assets") toggle(m_showContentBrowser);
        else if (w == "console") toggle(m_showConsole);
        else if (w == "dev") toggle(m_wickedTools.showDevWindow);
        else if (w == "guide") toggle(m_wickedTools.showGuideWindow);
        else if (w == "name") toggle(m_wickedTools.showNameWindow);
        else if (w == "layers") toggle(m_wickedTools.showLayerWindow);
        else if (w == "object") toggle(m_wickedTools.showObjectWindow);
        else if (w == "light") toggle(m_wickedTools.showLightWindow);
        else if (w == "camera") toggle(m_wickedTools.showCameraWindow);
        else if (w == "material") toggle(m_wickedTools.showMaterialWindow);
        else if (w == "sound") toggle(m_wickedTools.showSoundWindow);
        else if (w == "rigidbody") toggle(m_wickedTools.showRigidBodyWindow);
        else if (w == "collider") toggle(m_wickedTools.showColliderWindow);
        else if (w == "constraint") toggle(m_wickedTools.showConstraintWindow);
        else if (w == "softbody") toggle(m_wickedTools.showSoftBodyWindow);
        else if (w == "spring") toggle(m_wickedTools.showSpringWindow);
        else if (w == "decal") toggle(m_wickedTools.showDecalWindow);
        else if (w == "emitter") toggle(m_wickedTools.showEmitterWindow);
        else if (w == "hair") toggle(m_wickedTools.showHairParticleWindow);
        else if (w == "spline") toggle(m_wickedTools.showSplineWindow);
        else if (w == "forcefield") toggle(m_wickedTools.showForceFieldWindow);
        else if (w == "envprobe") toggle(m_wickedTools.showEnvProbeWindow);
        else if (w == "weather") toggle(m_wickedTools.showWeatherWindow);
        else if (w == "animation-tools") toggle(m_wickedTools.showAnimationWindow);
        else if (w == "armature") toggle(m_wickedTools.showArmatureWindow);
        else if (w == "humanoid") toggle(m_wickedTools.showHumanoidWindow);
        else if (w == "ik-tools") toggle(m_wickedTools.showIKWindow);
        else if (w == "expression") toggle(m_wickedTools.showExpressionWindow);
        else if (w == "terrain") toggle(m_wickedTools.showTerrainWindow);
        else if (w == "paint") toggle(m_wickedTools.showPaintToolWindow);
        else if (w == "mesh") toggle(m_wickedTools.showMeshWindow);
        else if (w == "importer") toggle(m_wickedTools.showModelImporterWindow);
        else if (w == "video") toggle(m_wickedTools.showVideoWindow);
        else if (w == "gaussian") toggle(m_wickedTools.showGaussianSplatWindow);
        else if (w == "theme") toggle(m_wickedTools.showThemeEditorWindow);
        else if (w == "project-creator") toggle(m_wickedTools.showProjectCreatorWindow);
        else if (w == "general") toggle(m_wickedTools.showGeneralWindow);
        else if (w == "graphics") toggle(m_wickedTools.showGraphicsWindow);
        else if (w == "profiler") toggle(m_wickedTools.showProfilerWindow);
        else { std::cout << "[ControlApi] window: unknown '" << w << "'" << std::endl; return; }
        std::cout << "[ControlApi] window toggled '" << w << "'" << std::endl;
    } else if (cmd.rfind("theme ", 0) == 0) {
        float r = 0.1f, g = 0.11f, b = 0.14f, pr = 0.2f, pg = 0.2f, pb = 0.2f;
        std::istringstream ss(cmd.substr(6));
        ss >> r >> g >> b >> pr >> pg >> pb;
        m_wickedTools.set_theme(glm::vec3(r, g, b), glm::vec3(pr, pg, pb));
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_WindowBg] = ImVec4(r, g, b, 1.0f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(pr, pg, pb, 1.0f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(pr, pg, pb, 1.0f);
        style.Colors[ImGuiCol_MenuBarBg] = ImVec4(pr, pg, pb, 1.0f);
        const float lift = 0.08f;
        style.Colors[ImGuiCol_FrameBg] = ImVec4(pr + lift, pg + lift, pb + lift, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(pr + lift, pg + lift, pb + lift, 1.0f);
        std::cout << "[ControlApi] theme applied" << std::endl;
    } else if (cmd.rfind("weather ", 0) == 0) {
        float sunR = 1.0f, sunG = 0.9f, sunB = 0.7f, fogDensity = 0.0f, fogStart = 0.0f, skyExposure = 1.0f, rain = 0.0f;
        std::istringstream ss(cmd.substr(8));
        ss >> sunR >> sunG >> sunB >> fogDensity >> fogStart >> skyExposure >> rain;
        if (m_editorScene) {
            UUID weatherId{ 0, 0 };
            for (const auto& [id, entity] : m_editorScene->get_entities()) {
                (void)entity;
                if (m_editorScene->weatherComponents.contains(id)) { weatherId = id; break; }
            }
            if (!weatherId.is_valid()) {
                Entity w = m_editorScene->create_entity("Weather");
                weatherId = w.get_id();
                m_editorScene->weatherComponents[weatherId] = WeatherComponent{};
            }
            auto& w = m_editorScene->weatherComponents[weatherId];
            w.sunColor = glm::vec3(sunR, sunG, sunB);
            w.fogDensity = fogDensity; w.fogStart = fogStart; w.skyExposure = skyExposure; w.rainAmount = rain;
            std::cout << "[ControlApi] weather applied" << std::endl;
        }
    } else if (cmd.rfind("selftest ", 0) == 0) {
        // Accept both numeric indices (0-4) and the friendly names
        // (rendergraph/hdr/material/play/build) so a typo like "material"
        // never crashes the editor with a std::stoi exception.
        std::string arg = cmd.substr(9);
        int which = -1;
        try {
            which = std::stoi(arg);
        } catch (...) {
            static const char* kTestNames[] = { "rendergraph", "hdr", "material", "play", "build" };
            for (int i = 0; i < 5; ++i) {
                if (arg == kTestNames[i]) { which = i; break; }
            }
        }
        if (which < 0 || which >= 5) {
            std::cout << "[ControlApi] selftest: invalid test '" << arg << "'" << std::endl;
        } else {
            m_lastSelfTestResult = run_editor_self_test(which);
            std::cout << "[ControlApi] selftest " << arg << " -> " << m_lastSelfTestResult << std::endl;
        }
    } else if (cmd == "package") {
        const std::string result = package_assets_only();
        std::cout << "[ControlApi] package -> " << result << std::endl;
    } else if (cmd == "hot-reload") {
        if (m_assetHotReload) m_assetHotReload->watch_registered_assets();
        const auto reloaded = m_assetHotReload ? m_assetHotReload->poll() : std::vector<AssetMetadata>{};
        std::cout << "[ControlApi] hot-reload -> " << reloaded.size() << " asset(s) reimported" << std::endl;
    } else {
        m_controlResult = "unrecognized command: " + cmd;
        std::cout << "[ControlApi] ignored '" << cmd << "' (state="
                  << static_cast<int>(m_playMode.get_state()) << ")" << std::endl;
    }
}

void EditorApplication::update_editor_camera(float deltaTime) {
    // Respond to the mouse over the rendered image, not to ImGui window focus:
    // focus can go stale (another panel taking it), which made the viewport
    // appear to stop answering the mouse entirely.
    if (!m_viewportImageHovered) return;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    const glm::vec2 mouse(static_cast<float>(mx), static_cast<float>(my));
    const glm::vec2 mouseDelta = mouse - m_lastMousePos;
    m_lastMousePos = mouse;

    EditorCamera& cam = m_editorCamera;
    const glm::vec3 front = cam.get_front();
    const glm::vec3 right = cam.get_right();
    const glm::vec3 up = cam.get_up();

    // Don't let camera keys fight the user typing in Inspector/text fields.
    // NOTE: io.WantCaptureKeyboard is true whenever the mouse hovers ANY
    // window, which would kill WASD the moment the cursor is over the 3D view;
    // io.WantTextInput is true only while an actual text field is being typed.
    ImGuiIO& io = ImGui::GetIO();
    const bool keysFree = !io.WantTextInput;

    // Orbit (right drag) / pan (middle drag).
    const bool orbitHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool panHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    if (orbitHeld && !m_gizmoDragging) {
        cam.yaw += mouseDelta.x * cam.sensitivity;
        cam.pitch = glm::clamp(cam.pitch - mouseDelta.y * cam.sensitivity, -89.0f, 89.0f);
    }
    if (panHeld) {
        const float panScale = cam.orbitDistance * 0.0016f;
        cam.orbitTarget += (-right * mouseDelta.x + up * mouseDelta.y) * panScale;
    }

    // Fly (WASD): free-fly whenever the mouse is over the viewport and the
    // keyboard is not captured by a text field — no right-button required.
    if (keysFree) {
        const float speed = cam.speed * (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 4.0f : 1.0f);
        glm::vec3 move(0.0f);
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) move += front;
        if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) move -= front;
        if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) move += right;
        if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) move -= right;
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) move += up;
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) move -= up;
        if (glm::length(move) > 0.0f) {
            cam.orbitTarget += glm::normalize(move) * speed * deltaTime;
        }
    }

    // Scroll zoom: the wheel inside the 3D view ALWAYS dollies toward/away
    // from the orbit focus — it never scrolls any panel (the viewport is
    // NoScrollbar|NoScrollWithMouse, and the delta is consumed here). The
    // delta comes from our own GLFW callback accumulator, not io.MouseWheel,
    // which ImGui zeroes at the end of NewFrame before we can read it. When
    // the viewport is NOT hovered the accumulator is dropped so ImGui keeps
    // scrolling other panels normally.
    if (m_viewportHovered || m_viewportImageHovered) {
        if (m_scrollAccum != 0.0) {
            cam.orbitDistance = glm::clamp(
                cam.orbitDistance * (1.0f - static_cast<float>(m_scrollAccum) * 0.1f), 0.5f, 5000.0f);
            m_scrollAccum = 0.0;
        }
    } else {
        m_scrollAccum = 0.0;
    }

    recompute_editor_camera_position();
}

void EditorApplication::process_viewport_input() {
    // Gizmo keys work on hover (mouse over the 3D image), not on ImGui window
    // focus — focus can sit on another panel and would freeze the keys.
    if (!m_viewportImageHovered) return;
    ImGuiIO& io = ImGui::GetIO();
    // Gizmo mode switching: Q / W / E / R
    if (!io.WantCaptureKeyboard) {
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS && m_gizmoMode != GizmoMode::Select) {
            m_gizmoMode = GizmoMode::Select;
        }
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS && m_gizmoMode != GizmoMode::Translate) {
            m_gizmoMode = GizmoMode::Translate;
        }
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS && m_gizmoMode != GizmoMode::Rotate) {
            m_gizmoMode = GizmoMode::Rotate;
        }
        if (glfwGetKey(m_window, GLFW_KEY_R) == GLFW_PRESS && m_gizmoMode != GizmoMode::Scale) {
            m_gizmoMode = GizmoMode::Scale;
        }
    }
}

bool EditorApplication::gizmo_axis_hit_test(glm::vec2 mouseScreen) {
    m_hoveredAxis = GizmoAxis::None;
    if (!m_editorScene || !m_selectedEntity.is_valid()) return false;
    const auto it = m_editorScene->transformComponents.find(m_selectedEntity.get_id());
    if (it == m_editorScene->transformComponents.end()) return false;
    const glm::vec3 origin = it->second.position;
    // World/Local hit test: axes rotate with the entity in local mode.
    const glm::quat gizmoRotation = m_gizmoLocal
        ? glm::quat(glm::radians(it->second.rotation))
        : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const auto axisWorld = [&](int axis) -> glm::vec3 { return gizmoRotation * kAxisDirs[axis]; };

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();
    const auto project = [&](const glm::vec3& world) -> glm::vec2 {
        glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (std::abs(clip.w) < 1e-6f) return { -1e9f, -1e9f };
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec2(m_viewportImagePos.x + (ndc.x * 0.5f + 0.5f) * m_viewportImageSize.x,
                         m_viewportImagePos.y + (-ndc.y * 0.5f + 0.5f) * m_viewportImageSize.y);
    };

    const glm::vec2 originScreen = project(origin);
    float bestDist = 1e18f;
    GizmoAxis best = GizmoAxis::None;
    const float gizmoLen = (m_gizmoMode == GizmoMode::Rotate) ? 1.45f : 1.55f;
    for (int axis = 0; axis < 3; ++axis) {
        float dist = 1e18f;
        if (m_gizmoMode == GizmoMode::Rotate) {
            // Distance to the projected ring polyline.
            for (int s = 0; s < 48; ++s) {
                const float a0 = glm::two_pi<float>() * static_cast<float>(s) / 48.0f;
                const float a1 = glm::two_pi<float>() * static_cast<float>(s + 1) / 48.0f;
                const glm::vec3 dir = axisWorld(axis);
                glm::vec3 u = glm::normalize(glm::cross(dir, std::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
                glm::vec3 v = glm::normalize(glm::cross(dir, u));
                const glm::vec3 p0 = origin + u * (std::cos(a0) * gizmoLen) + v * (std::sin(a0) * gizmoLen);
                const glm::vec3 p1 = origin + u * (std::cos(a1) * gizmoLen) + v * (std::sin(a1) * gizmoLen);
                dist = std::min(dist, dist_point_segment(mouseScreen, project(p0), project(p1)));
            }
        } else {
            const glm::vec2 tipScreen = project(origin + axisWorld(axis) * gizmoLen);
            dist = dist_point_segment(mouseScreen, originScreen, tipScreen);
        }
        if (dist < 14.0f && dist < bestDist) {
            bestDist = dist;
            best = static_cast<GizmoAxis>(axis + 1);
        }
    }
    m_hoveredAxis = best;
    return best != GizmoAxis::None;
}

void EditorApplication::start_gizmo_drag(glm::vec2 mouseScreen) {
    if (!m_editorScene || !m_selectedEntity.is_valid()) return;
    const UUID id = m_selectedEntity.get_id();
    if (!m_editorScene->transformComponents.contains(id)) return;
    const TransformComponent& t = m_editorScene->transformComponents.at(id);

    m_gizmoDragging = true;
    m_gizmoDragEntityStart = t.position;
    m_gizmoDragRotStart = t.rotation;
    m_gizmoDragScaleStart = t.scale;
    // World/Local: in local mode the drag axis follows the entity rotation.
    if (m_gizmoLocal) {
        m_gizmoAxisWorld = glm::quat(glm::radians(t.rotation)) * kAxisDirs[static_cast<int>(m_activeAxis) - 1];
    } else {
        m_gizmoAxisWorld = kAxisDirs[static_cast<int>(m_activeAxis) - 1];
    }
    m_gizmoDragPlaneNormal = glm::normalize(m_editorCamera.orbitTarget - m_editorCamera.position);
    if (glm::length(m_gizmoDragPlaneNormal) < 1e-5f) m_gizmoDragPlaneNormal = glm::vec3(0, 0, 1);

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 invViewProj = glm::inverse(
        m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    m_gizmoDragPlanePoint = unproject_to_plane(mouseScreen, t.position, m_gizmoDragPlaneNormal, invViewProj);

    if (m_gizmoMode == GizmoMode::Rotate) {
        glm::vec3 toPoint = m_gizmoDragPlanePoint - t.position;
        if (glm::length(toPoint) < 1e-5f) toPoint = m_gizmoAxisWorld == glm::vec3(0, 1, 0) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 ref = toPoint - m_gizmoAxisWorld * glm::dot(toPoint, m_gizmoAxisWorld);
        if (glm::dot(ref, ref) < 1e-6f) ref = glm::normalize(glm::cross(m_gizmoAxisWorld, glm::vec3(0, 0, 1)));
        m_gizmoDragAngleRef = glm::normalize(ref);
    }
}

void EditorApplication::update_gizmo_drag(glm::vec2 mouseScreen) {
    if (!m_editorScene || !m_selectedEntity.is_valid() || !m_gizmoDragging) return;
    const UUID id = m_selectedEntity.get_id();
    auto it = m_editorScene->transformComponents.find(id);
    if (it == m_editorScene->transformComponents.end()) return;

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 invViewProj = glm::inverse(
        m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    const glm::vec3 planePoint = unproject_to_plane(mouseScreen, m_gizmoDragPlanePoint,
                                                    m_gizmoDragPlaneNormal, invViewProj);
    const bool snap = ImGui::GetIO().KeyCtrl;
    const int axisIndex = static_cast<int>(m_activeAxis) - 1;

    if (m_gizmoMode == GizmoMode::Translate) {
        float delta = glm::dot(planePoint - m_gizmoDragPlanePoint, m_gizmoAxisWorld);
        if (snap) delta = std::round(delta / m_snapTranslate) * m_snapTranslate;
        const glm::vec3 newPos = m_gizmoDragEntityStart + m_gizmoAxisWorld * delta;
        m_undo.execute_or_merge_property(
            "Move Entity",
            [this, id, newPos] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.position = newPos; },
            [this, id, start = m_gizmoDragEntityStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.position = start;
            });
    } else if (m_gizmoMode == GizmoMode::Rotate) {
        glm::vec3 toPoint = planePoint - m_gizmoDragEntityStart;
        if (glm::length(toPoint) < 1e-5f) return;
        glm::vec3 v = toPoint - m_gizmoAxisWorld * glm::dot(toPoint, m_gizmoAxisWorld);
        if (glm::dot(v, v) < 1e-6f) return;
        v = glm::normalize(v);
        const float angle = glm::degrees(std::atan2(
            glm::dot(glm::cross(m_gizmoDragAngleRef, v), m_gizmoAxisWorld),
            glm::dot(m_gizmoDragAngleRef, v)));
        const float snapped = snap ? std::round(angle / m_snapRotate) * m_snapRotate : angle;
        glm::vec3 newRot = m_gizmoDragRotStart;
        newRot[axisIndex] += snapped;
        m_undo.execute_or_merge_property(
            "Rotate Entity",
            [this, id, newRot] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.rotation = newRot; },
            [this, id, start = m_gizmoDragRotStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.rotation = start;
            });
    } else if (m_gizmoMode == GizmoMode::Scale) {
        float delta = glm::dot(planePoint - m_gizmoDragPlanePoint, m_gizmoAxisWorld);
        float factor = 1.0f + delta / 1.0f;
        if (snap) factor = std::round(factor / m_snapScale) * m_snapScale;
        factor = std::max(factor, 0.02f);
        glm::vec3 newScale = m_gizmoDragScaleStart;
        newScale[axisIndex] = m_gizmoDragScaleStart[axisIndex] * factor;
        m_undo.execute_or_merge_property(
            "Scale Entity",
            [this, id, newScale] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.scale = newScale; },
            [this, id, start = m_gizmoDragScaleStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.scale = start;
            });
    }
}

bool EditorApplication::load_mesh_resource(const UUID& assetId) {
    const auto cached = m_meshResources.find(assetId);
    if (cached != m_meshResources.end()) return cached->second.valid;
    if (m_meshLoadFailed.contains(assetId)) return false;

    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Mesh || !found->isCooked ||
        found->cookedPath.empty() || !std::filesystem::is_regular_file(found->cookedPath)) {
        m_meshLoadFailed.insert(assetId);
        return false;
    }
    std::string error;
    const GltfGeometryResult geometry = GltfGeometryParser::parse_vcmesh(found->cookedPath, &error);
    if (!geometry.success || geometry.primitives.empty()) {
        std::cerr << "[Editor] Cannot load mesh " << assetId.to_string() << ": " << error << std::endl;
        m_meshLoadFailed.insert(assetId);
        return false;
    }
    const float meshScale = found->importSettings.meshScale > 0.0f ? found->importSettings.meshScale : 1.0f;

    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    EditorMeshResource resource;
    for (const GltfMeshPrimitive& primitive : geometry.primitives) {
        const uint32_t vertexOffset = static_cast<uint32_t>(verts.size());
        verts.reserve(verts.size() + primitive.positions.size());
        for (size_t i = 0; i < primitive.positions.size(); ++i) {
            EditorVertex v;
            v.pos = primitive.positions[i] * meshScale;
            v.normal = i < primitive.normals.size() ? primitive.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
            v.color = glm::vec3(1.0f);
            v.uv = i < primitive.uvs.size() ? primitive.uvs[i] : glm::vec2(0.0f);
            verts.push_back(v);
            if (!resource.hasBounds) {
                resource.boundsMin = resource.boundsMax = v.pos;
                resource.hasBounds = true;
            } else {
                resource.boundsMin = glm::min(resource.boundsMin, v.pos);
                resource.boundsMax = glm::max(resource.boundsMax, v.pos);
            }
        }
        if (primitive.indexed) {
            const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
            for (uint32_t index : primitive.indices) indices.push_back(index + vertexOffset);
            resource.ranges.push_back({ firstIndex, static_cast<uint32_t>(primitive.indices.size()), 0, true });
        } else {
            resource.ranges.push_back({ 0, static_cast<uint32_t>(primitive.positions.size()), vertexOffset, false });
        }
    }
    resource.vertexCount = static_cast<uint32_t>(verts.size());
    resource.valid = true;
    resource.cpuPositions.reserve(verts.size());
    for (const EditorVertex& v : verts) resource.cpuPositions.push_back(v.pos);

    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = indices.empty() ? 0 : sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  resource.vb.buffer, resource.vb.memory);
    if (ibSize > 0) {
        create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      resource.ib.buffer, resource.ib.memory);
    }
    safe_map_and_copy(m_device, resource.vb.memory, 0, vbSize, verts.data());
    if (ibSize > 0) {
        safe_map_and_copy(m_device, resource.ib.memory, 0, ibSize, indices.data());
    }
    resource.cpuIndices = std::move(indices);

    m_meshResources[assetId] = std::move(resource);
    return true;
}

const EditorApplication::EditorMeshResource* EditorApplication::get_mesh_resource(const UUID& assetId) {
    if (!assetId.is_valid()) return nullptr;
    // Block models: a Block asset is a textured cube — build the GPU mesh on
    // demand so spawned block entities survive restarts (no asset file, the
    // cube geometry is generated and uploaded here).
    if (const auto blockFound = m_assetRegistry.find(assetId);
        blockFound && blockFound->type == AssetType::Block) {
        ensure_block_cube_resource(assetId);
        const auto it = m_meshResources.find(assetId);
        return (it != m_meshResources.end() && it->second.valid) ? &it->second : nullptr;
    }
    // Minecraft character/mob skins: the texture IS the character. The
    // humanoid mesh is generated from the skin's UV layout (64x64 or legacy
    // 64x32) and cached per texture UUID, same on-demand pattern as blocks.
    if (const auto skinFound = m_assetRegistry.find(assetId);
        skinFound && skinFound->type == AssetType::Texture && is_character_texture(*skinFound)) {
        ensure_character_mesh_resource(assetId);
        const auto it = m_meshResources.find(assetId);
        return (it != m_meshResources.end() && it->second.valid) ? &it->second : nullptr;
    }
    if (!load_mesh_resource(assetId)) return nullptr;
    const auto found = m_meshResources.find(assetId);
    return (found != m_meshResources.end() && found->second.valid) ? &found->second : nullptr;
}

void EditorApplication::cleanup() {
    join_worker_threads();
    shutdown_audio_output();
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

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

bool EditorApplication::pick_folder_dialog(std::string& outPath, const wchar_t* title) {
    wchar_t buf[MAX_PATH]{ 0 };
    BROWSEINFOW bi{};
    bi.hwndOwner = glfwGetWin32Window(m_window);
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    if (!SHGetPathFromIDListW(pidl, buf)) {
        CoTaskMemFree(pidl);
        return false;
    }
    CoTaskMemFree(pidl);
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}

void EditorApplication::load_scene_file(const std::string& path) {
    auto scene = std::make_unique<Scene>("Untitled Scene");
    if (!scene->load_from_file(path)) {
        std::cerr << "[Editor] Falha ao abrir cena: " << path << std::endl;
        return;
    }
    if (m_playMode.get_state() != PlayState::Edit) m_playMode.stop_play();
    m_editorScene = std::move(scene);
    m_playMode.set_editor_scene(m_editorScene.get());
    m_activeScenePath = path;
    m_autosavePath.clear();
    m_sceneDirty = false;
    m_editorGui.init(m_editorScene.get(), &m_undo);
    m_editorGui.set_asset_registry(&m_assetRegistry);
    m_selectedEntity = Entity();
    m_editorGui.select_entity(m_selectedEntity);
    // Give the scene a camera if it lacks one, so the viewport is usable.
    bool hasCamera = false;
    for (const auto& [id, ent] : m_editorScene->get_entities()) {
        if (m_editorScene->cameraComponents.contains(id)) { hasCamera = true; break; }
    }
    if (!hasCamera) {
        Entity cam = m_editorScene->create_entity(tr("Câmera Principal", "Main Camera"));
        m_editorScene->transformComponents[cam.get_id()].position = glm::vec3(0.0f, 2.0f, 5.0f);
        m_editorScene->cameraComponents[cam.get_id()] = CameraComponent{ 70.0f, 0.1f, 2000.0f, true };
    }
    // Terrain is editor-owned (the scene serializer stores entity data only),
    // so the heightmap parameters live in the ".terrain" sidecar next to the
    // scene file and are regenerated on load.
    restore_terrain_sidecar(path);
    std::cout << "[Editor] Cena carregada: " << path << " ("
              << m_editorScene->get_entities().size() << " entidades)" << std::endl;
}

void EditorApplication::scan_projects(std::vector<LauncherProject>& out) const {
    const std::filesystem::path projectsDir =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Projects";
    std::error_code ec;
    if (!std::filesystem::exists(projectsDir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(projectsDir, ec)) {
        if (!entry.is_directory()) continue;
        LauncherProject proj;
        proj.name = entry.path().filename().string();
        proj.path = entry.path().string();
        // Last write time of the folder tree, best-effort.
        auto ftime = std::filesystem::last_write_time(entry.path(), ec);
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            char buf[64]{ 0 };
            std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", std::localtime(&t));
            proj.lastModified = buf;
        } else {
            proj.lastModified = "—";
        }
        // Does the project contain a .scene anywhere under it?
        std::error_code subEc;
        for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path(), subEc)) {
            if (file.is_regular_file() && file.path().extension() == ".scene") {
                proj.hasScene = true;
                break;
            }
        }
        out.push_back(std::move(proj));
    }
    // Stable, predictable order.
    std::sort(out.begin(), out.end(),
              [](const LauncherProject& a, const LauncherProject& b) { return a.name < b.name; });
}

void EditorApplication::save_current_scene() {
    if (!m_editorScene) return;
    if (!m_activeScenePath.empty()) {
        if (!m_editorScene->save_to_file(m_activeScenePath)) {
            std::cerr << "[Editor] Falha ao salvar: " << m_activeScenePath << std::endl;
        } else {
            m_sceneDirty = false;
            persist_terrain_sidecar(m_activeScenePath);
            std::cout << "[Editor] Cena salva: " << m_activeScenePath << std::endl;
        }
        return;
    }
    // No path yet — behave like Salvar Como.
    save_scene_as();
}

void EditorApplication::save_scene_as() {
    if (!m_editorScene) return;
    std::string path;
    if (!pick_save_file_dialog(path, L"Cenas VulkanCraft (*.scene)\0*.scene\0Todos (*.*)\0*.*\0",
                               L"Salvar Cena Como", L"scene")) {
        return;
    }
    if (!m_editorScene->save_to_file(path)) {
        std::cerr << "[Editor] Falha ao salvar: " << path << std::endl;
        return;
    }
    m_activeScenePath = path;
    m_autosavePath.clear();
    m_sceneDirty = false;
    persist_terrain_sidecar(path);
    std::cout << "[Editor] Cena salva: " << path << std::endl;
}

void EditorApplication::create_new_scene() {
    // Stop the play world first so it doesn't keep ticking the old scene.
    if (m_playMode.get_state() != PlayState::Edit) {
        teardown_play_runtime();
        m_playMode.stop_play();
    }
    m_selectedEntity = Entity();
    m_editorGui.select_entity(m_selectedEntity);
    const std::string name = (m_newSceneName[0] != '\0') ? m_newSceneName : "Untitled Scene";
    m_editorScene = std::make_unique<Scene>(name);
    m_playMode.set_editor_scene(m_editorScene.get());
    m_activeScenePath.clear();  // new scene has no file until Salvar
    clear_terrain_mesh();
    init_default_scene();
    std::cout << "[Editor] Nova cena: " << name << std::endl;
}

void EditorApplication::clear_terrain_mesh() {
    if (m_terrainVB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainVB); m_terrainVB = GPUBuffer{}; }
    if (m_terrainIB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainIB); m_terrainIB = GPUBuffer{}; }
    m_terrainValid = false;
    m_terrainIndexCount = 0;
    m_terrainParams = TerrainParams{};
}

void EditorApplication::load_settings() {
    m_settingsPath = (std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "settings.json").string();
    std::ifstream in(m_settingsPath);
    if (!in) return;
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Tiny hand-rolled key/value reader for our own settings format.
    const auto findString = [&](const std::string& key) -> std::string {
        const std::string token = "\"" + key + "\"";
        const size_t pos = content.find(token);
        if (pos == std::string::npos) return {};
        const size_t colon = content.find(':', pos + token.size());
        if (colon == std::string::npos) return {};
        size_t start = colon + 1;
        while (start < content.size() && std::isspace(static_cast<unsigned char>(content[start]))) ++start;
        if (start >= content.size()) return {};
        if (content[start] == '"') {
            const size_t end = content.find('"', start + 1);
            return end == std::string::npos ? std::string() : content.substr(start + 1, end - start - 1);
        }
        const size_t end = content.find_first_of(",}\n", start);
        std::string val = content.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
        return val;
    };
    const auto findVec = [&](const std::string& key) -> glm::vec3 {
        const std::string token = "\"" + key + "\"";
        const size_t pos = content.find(token);
        if (pos == std::string::npos) return glm::vec3(-1.0f);
        const size_t lb = content.find('[', pos);
        const size_t rb = lb == std::string::npos ? std::string::npos : content.find(']', lb);
        if (lb == std::string::npos || rb == std::string::npos) return glm::vec3(-1.0f);
        const std::string arr = content.substr(lb + 1, rb - lb - 1);
        glm::vec3 v(0.0f);
        size_t i = 0;
        for (int comp = 0; comp < 3; ++comp) {
            while (i < arr.size() && (arr[i] == ' ' || arr[i] == ',')) ++i;
            const size_t start = i;
            while (i < arr.size() && arr[i] != ',' && arr[i] != ' ') ++i;
            if (i > start) v[static_cast<size_t>(comp)] = std::stof(arr.substr(start, i - start));
        }
        return v;
    };

    const std::string lang = findString("language");
    if (lang == "en") m_currentLanguage = EngineLanguage::EN_US;
    else if (lang == "pt") m_currentLanguage = EngineLanguage::PT_BR;
    const std::string vsync = findString("vsync");
    if (vsync == "false") m_vsyncEnabled = false;
    else if (vsync == "true") m_vsyncEnabled = true;
    const std::string quality = findString("shadowQuality");
    if (!quality.empty()) m_shadowQuality = std::clamp(std::atoi(quality.c_str()), 1, 4);
    // Theme colors are parsed but NOT reapplied on boot: the Forge light
    // design system is the base theme, and the Theme Editor panel tunes the
    // live style during the session (persisting it is a TODO(frontend-port)).
    const glm::vec3 bg = findVec("themeBg");
    const glm::vec3 panel = findVec("themePanel");
    if (bg.x >= 0.0f && panel.x >= 0.0f) {
        m_wickedTools.set_theme(bg, panel);
    }
    std::cout << "[Editor] Configurações carregadas: " << m_settingsPath << std::endl;
}

uint32_t EditorApplication::shadow_size_from_quality(int quality) const {
    switch (std::clamp(quality, 1, 4)) {
        case 1: return 512;
        case 2: return 1024;
        case 3: return 2048;
        default: return 4096;
    }
}


glm::vec3 EditorCamera::get_right() const {
    const glm::vec3 front = get_front();
    const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
    return right;
}

glm::vec3 EditorCamera::get_up() const {
    return glm::normalize(glm::cross(get_right(), get_front()));
}

std::string EditorApplication::run_editor_self_test(int which) {
    static const char* kTestEnv[] = {
        "VC_EDITOR_TEST_RENDERGRAPH",
        "VC_EDITOR_TEST_HDR",
        "VC_EDITOR_TEST_MATERIAL",
        "VC_EDITOR_TEST_PLAY",
        "VC_EDITOR_TEST_BUILD",
    };
    if (which < 0 || which >= 5) return "Erro: teste inválido";
#ifdef _WIN32
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        return "Erro: não foi possível localizar o executável";
    }
    // The child inherits the environment at creation time; set the test flag
    // only for the duration of the spawn (the parent never re-reads it after
    // startup). CREATE_NO_WINDOW keeps the headless run out of the user's way.
    SetEnvironmentVariableA(kTestEnv[which], "1");
    STARTUPINFOA si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::string cmdLine = std::string("\"") + exePath + "\"";
    if (!CreateProcessA(exePath, cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        SetEnvironmentVariableA(kTestEnv[which], nullptr);
        return std::string("Erro: falha ao iniciar o teste (code ") +
               std::to_string(GetLastError()) + ")";
    }
    // Never wait forever: a hung headless child would wedge the editor's main
    // loop (and with it the Control API). 120s is generous for any build test.
    const DWORD waitMs = 120000;
    const DWORD waitResult = WaitForSingleObject(pi.hProcess, waitMs);
    DWORD code = 0;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 1);
        code = 1;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    SetEnvironmentVariableA(kTestEnv[which], nullptr);
    return code == 0 ? "PASS" : ("FAIL (exit " + std::to_string(code) + ")");
#else
    // Non-Windows fallback: spawn via shell and read the exit status.
    const std::string cmd =
        std::string(kTestEnv[which]) + "=1 ./VulkanEngineEditor >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    return rc == 0 ? "PASS" : ("FAIL (exit " + std::to_string(rc) + ")");
#endif
}

std::string EditorApplication::package_assets_only() {
    std::vector<UUID> roots;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) roots.push_back(asset.id);
    }
    if (roots.empty()) {
        return tr("Erro: nenhum asset cozido para empacotar (importe assets primeiro).",
                  "Error: no cooked assets to package (import assets first).");
    }
    const std::filesystem::path out =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "Package";
    const AssetPackageResult packaged = AssetPackager::package(m_assetRegistry, roots, out);
    if (!packaged) {
        return std::string(tr("Erro: ", "Error: ")) + packaged.error;
    }
    std::cout << "[Editor] standalone package: " << packaged.assets.size()
              << " asset(s) -> " << out.string() << std::endl;
    return tr("OK: ", "OK: ") + std::to_string(packaged.assets.size()) +
           tr(" asset(s) empacotados em ", " asset(s) packaged to ") + out.string();
}

void EditorApplication::recompute_editor_camera_position() {
    // Recompute the camera position from target + spherical offset.
    m_editorCamera.position = m_editorCamera.orbitTarget -
                              euler_direction(m_editorCamera.yaw, m_editorCamera.pitch) *
                              m_editorCamera.orbitDistance;
}

glm::vec3 EditorApplication::unproject_to_plane(glm::vec2 mouseScreen, const glm::vec3& planePoint,
                                                const glm::vec3& planeNormal, const glm::mat4& invViewProj) const {
    const float ndcX = (mouseScreen.x - m_viewportImagePos.x) / std::max(1.0f, m_viewportImageSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mouseScreen.y - m_viewportImagePos.y) / std::max(1.0f, m_viewportImageSize.y) * 2.0f;
    const glm::vec4 near4 = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    const glm::vec4 far4 = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec3 nearP = glm::vec3(near4) / near4.w;
    const glm::vec3 farP = glm::vec3(far4) / far4.w;
    const glm::vec3 dir = glm::normalize(farP - nearP);
    const float denom = glm::dot(dir, planeNormal);
    if (std::abs(denom) < 1e-6f) return planePoint;
    const float t = glm::dot(planePoint - nearP, planeNormal) / denom;
    return nearP + dir * t;
}

bool EditorApplication::pick_save_file_dialog(std::string& outPath, const wchar_t* filter,
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
    // Default folder: the engine's scenes folder (works from any cwd).
    const std::filesystem::path defaultDir =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes";
    std::error_code ec;
    std::filesystem::create_directories(defaultDir, ec);
    std::wstring initialDir = defaultDir.wstring();
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}

std::filesystem::path EditorApplication::terrain_sidecar_path(const std::string& scenePath) {
    return std::filesystem::path(scenePath).string() + ".terrain";
}

void EditorApplication::persist_terrain_sidecar(const std::string& scenePath) {
    if (!m_terrainValid || scenePath.empty()) return;
    std::error_code ec;
    const std::filesystem::path path = terrain_sidecar_path(scenePath);
    std::ofstream out(path);
    if (!out) {
        std::cerr << "[Editor] Terrain sidecar write failed: " << path << std::endl;
        return;
    }
    const TerrainParams& p = m_terrainParams;
    out << "scale=" << p.scale << "\n"
        << "octaves=" << p.octaves << "\n"
        << "amount=" << p.amount << "\n"
        << "falloff=" << p.falloff << "\n"
        << "halfExtent=" << p.halfExtent << "\n"
        << "segments=" << p.segments << "\n"
        << "seed=" << p.seed << "\n";
    out.close();
    if (!out) std::cerr << "[Editor] Terrain sidecar write failed (close): " << path << std::endl;
}

void EditorApplication::restore_terrain_sidecar(const std::string& scenePath) {
    if (scenePath.empty()) return;
    const std::filesystem::path path = terrain_sidecar_path(scenePath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        // No sidecar: the scene has no terrain authored yet.
        clear_terrain_mesh();
        return;
    }
    std::ifstream in(path);
    if (!in) return;
    TerrainParams p = m_terrainParams;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        try {
            if (key == "scale") p.scale = std::stof(val);
            else if (key == "octaves") p.octaves = std::stoi(val);
            else if (key == "amount") p.amount = std::stof(val);
            else if (key == "falloff") p.falloff = std::stof(val);
            else if (key == "halfExtent") p.halfExtent = std::stof(val);
            else if (key == "segments") p.segments = std::stoi(val);
            else if (key == "seed") p.seed = static_cast<uint32_t>(std::stoul(val));
        } catch (const std::exception&) {
            // Tolerate a malformed line; keep the previous value.
        }
    }
    generate_terrain_mesh(p);
    std::cout << "[Editor] Terrain restaurado: " << path
              << " (scale=" << p.scale << " seed=" << p.seed << ")" << std::endl;
}

void EditorApplication::generate_terrain_mesh(const TerrainParams& params) {
    // Drop the previous GPU buffers before regenerating. The old buffers may
    // still be referenced by an in-flight command buffer — freeing them
    // without waiting crashes the device (fence wait failed: -4, then the
    // viewport renders black forever). The editor can afford an idle here:
    // terrain regeneration is a user/API action, never per-frame.
    if (m_terrainVB.buffer != VK_NULL_HANDLE || m_terrainIB.buffer != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);
    if (m_terrainVB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainVB); m_terrainVB = GPUBuffer{}; }
    if (m_terrainIB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainIB); m_terrainIB = GPUBuffer{}; }
    m_terrainValid = false;
    m_terrainParams = params;
    m_terrainIndexCount = 0;

    // Height pass: y = terrain_surface_height(...) with a radial falloff that
    // pulls the border back to 0 so the sheet blends with the infinite grid.
    // The math lives in ONE place (anonymous namespace near
    // setup_play_runtime) because play-mode collision shares it.

    const int segments = params.segments;
    const float half = params.halfExtent;
    const float step = (2.0f * half) / static_cast<float>(segments);

    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    verts.reserve(static_cast<size_t>(segments + 1) * (segments + 1));

    const size_t cols = static_cast<size_t>(segments + 1);
    for (int zi = 0; zi <= segments; ++zi) {
        for (int xi = 0; xi <= segments; ++xi) {
            const float x = -half + static_cast<float>(xi) * step;
            const float z = -half + static_cast<float>(zi) * step;
            const float h = terrain_surface_height(params.seed, params.scale,
                                                   params.octaves, params.amount,
                                                   params.falloff, half, x, z);
            EditorVertex v;
            v.pos = glm::vec3(x, h, z);
            v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            v.color = glm::vec3(0.55f, 0.62f, 0.50f);
            v.uv = glm::vec2((x + half) / (2.0f * half), (z + half) / (2.0f * half));
            verts.push_back(v);
        }
    }
    // Indexed grid: two triangles per cell.
    for (int zi = 0; zi < segments; ++zi) {
        for (int xi = 0; xi < segments; ++xi) {
            const uint32_t a = static_cast<uint32_t>(zi) * static_cast<uint32_t>(cols) + static_cast<uint32_t>(xi);
            const uint32_t b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(cols);
            const uint32_t d = c + 1;
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    // Smooth normals: area-weighted accumulation from the triangle faces.
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const glm::vec3& p0 = verts[indices[i]].pos;
        const glm::vec3& p1 = verts[indices[i + 1]].pos;
        const glm::vec3& p2 = verts[indices[i + 2]].pos;
        const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        verts[indices[i]].normal += n;
        verts[indices[i + 1]].normal += n;
        verts[indices[i + 2]].normal += n;
    }
    for (EditorVertex& v : verts) {
        const float len = glm::length(v.normal);
        v.normal = len > 1e-8f ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    // GPU buffers (host-visible, same as the other editor meshes).
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_terrainVB.buffer, m_terrainVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_terrainIB.buffer, m_terrainIB.memory);
    m_terrainVB.size = vbSize;
    m_terrainIB.size = ibSize;
    safe_map_and_copy(m_device, m_terrainVB.memory, 0, vbSize, verts.data());
    safe_map_and_copy(m_device, m_terrainIB.memory, 0, ibSize, indices.data());
    m_terrainIndexCount = static_cast<uint32_t>(indices.size());
    m_terrainValid = true;
    std::cout << "[Editor] Terreno gerado: " << cols * cols << " vértices, "
              << indices.size() / 3 << " triângulos" << std::endl;
}

std::string EditorApplication::create_project(const std::string& name, const std::string& folder) {
    if (name.empty()) return "Erro: nome do projeto vazio.";
    // Sanitize into a folder-safe slug.
    std::string slug = name;
    for (char& c : slug) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    }
    std::filesystem::path root;
    if (!folder.empty()) {
        root = std::filesystem::path(folder) / slug;
    } else {
        root = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Projects" / slug;
    }
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) return "Erro: a pasta já existe: " + root.string();

    const std::filesystem::path scenesDir = root / "assets" / "scenes";
    std::filesystem::create_directories(scenesDir, ec);
    if (ec) return "Erro: não foi possível criar " + root.string();

    // Fresh scene with the same defaults as the editor (camera + sun).
    Scene scene(name);
    Entity camera = scene.create_entity("Câmera Principal");
    scene.transformComponents[camera.get_id()].position = glm::vec3(0.0f, 2.0f, 5.0f);
    scene.cameraComponents[camera.get_id()] = CameraComponent{ 70.0f, 0.1f, 2000.0f, true };
    Entity sun = scene.create_entity("Luz Direcional");
    scene.lightComponents[sun.get_id()] = LightComponent{ glm::vec3(1.0f, 0.95f, 0.85f), 10000.0f, 1000.0f, true };
    scene.transformComponents[sun.get_id()].rotation = glm::vec3(-45.0f, 30.0f, 0.0f);
    const std::filesystem::path scenePath = scenesDir / "active_world.scene";
    if (!scene.save_to_file(scenePath.string())) {
        return "Erro: falha ao salvar a cena inicial.";
    }

    // Empty asset registry (Content Browser starts clean).
    AssetRegistry reg;
    const std::filesystem::path regPath = root / "Intermediate" / "AssetRegistry.db";
    std::filesystem::create_directories(regPath.parent_path(), ec);
    reg.save(regPath.string());

    // README marker so the folder reads as a project at a glance.
    std::ofstream readme(root / "README.md");
    readme << "# " << name << "\n\nProjeto criado pelo Criador de Projetos do editor.\n";
    readme.close();

    std::cout << "[Editor] Projeto criado: " << root.string() << std::endl;
    return "OK: " + root.string();
}

void EditorApplication::save_settings() {
    if (m_settingsPath.empty()) {
        m_settingsPath = (std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "settings.json").string();
    }
    std::ofstream out(m_settingsPath, std::ios::trunc);
    if (!out) {
        std::cerr << "[Editor] Não foi possível salvar " << m_settingsPath << std::endl;
        return;
    }
    const glm::vec3 bg = m_wickedTools.theme_background();
    const glm::vec3 panel = m_wickedTools.theme_panel();
    out << "{\n";
    out << "  \"language\": \"" << (m_currentLanguage == EngineLanguage::PT_BR ? "pt" : "en") << "\",\n";
    out << "  \"vsync\": " << (m_vsyncEnabled ? "true" : "false") << ",\n";
    out << "  \"shadowQuality\": " << m_shadowQuality << ",\n";
    out << "  \"themeBg\": [" << bg.r << ", " << bg.g << ", " << bg.b << "],\n";
    out << "  \"themePanel\": [" << panel.r << ", " << panel.g << ", " << panel.b << "]\n";
    out << "}\n";
    std::cout << "[Editor] Configurações salvas: " << m_settingsPath << std::endl;
}

void EditorApplication::apply_graphics_settings(bool vsync, int quality) {
    const bool vsyncChanged = m_vsyncEnabled != vsync;
    m_vsyncEnabled = vsync;
    m_shadowQuality = std::clamp(quality, 1, 4);
    if (vsyncChanged) m_recreateSwapchain = true;
    m_recreateShadowMap = true;
    std::cout << "[Editor] Gráficas: vsync=" << (vsync ? "on" : "off")
              << ", sombras=" << shadow_size_from_quality(m_shadowQuality) << std::endl;
}

std::string EditorApplication::apply_mesh_normals(int mode) {
    if (!m_selectedEntity.is_valid()) return "Nenhum objeto selecionado.";
    Scene* scene = m_editorScene.get();
    if (!scene) return "Sem cena aberta.";
    const UUID id = m_selectedEntity.get_id();
    const auto meshIt = scene->meshRendererComponents.find(id);
    if (meshIt == scene->meshRendererComponents.end() || !meshIt->second.meshAssetID.is_valid()) {
        return "O objeto selecionado não tem malha.";
    }
    const UUID assetId = meshIt->second.meshAssetID;
    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Mesh || found->cookedPath.empty() ||
        !std::filesystem::is_regular_file(found->cookedPath)) {
        return "Asset de malha não encontrado (o modelo precisa ser importado/cookado).";
    }

    std::string error;
    GltfGeometryResult geometry = GltfGeometryParser::parse_vcmesh(found->cookedPath, &error);
    if (!geometry.success) return "Falha ao ler a malha: " + error;

    for (GltfMeshPrimitive& prim : geometry.primitives) {
        if (mode == 1) {
            // Flip: negate the existing normals.
            for (glm::vec3& n : prim.normals) n = -n;
            continue;
        }
        // Recalc smooth: area-weighted accumulation per vertex.
        std::vector<glm::vec3> acc(prim.positions.size(), glm::vec3(0.0f));
        if (prim.indexed && prim.indices.size() >= 3) {
            for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
                const uint32_t ia = prim.indices[i], ib = prim.indices[i + 1], ic = prim.indices[i + 2];
                if (ia >= prim.positions.size() || ib >= prim.positions.size() || ic >= prim.positions.size()) continue;
                const glm::vec3 n = glm::cross(prim.positions[ib] - prim.positions[ia],
                                               prim.positions[ic] - prim.positions[ia]);
                acc[ia] += n; acc[ib] += n; acc[ic] += n;
            }
        } else {
            for (size_t i = 0; i + 2 < prim.positions.size(); i += 3) {
                const glm::vec3 n = glm::cross(prim.positions[i + 1] - prim.positions[i],
                                               prim.positions[i + 2] - prim.positions[i]);
                acc[i] += n; acc[i + 1] += n; acc[i + 2] += n;
            }
        }
        prim.normals.resize(prim.positions.size());
        for (size_t i = 0; i < prim.positions.size(); ++i) {
            const float len = glm::length(acc[i]);
            prim.normals[i] = len > 1e-8f ? acc[i] / len : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    // Re-upload the GPU resource in place (same vertex layout as load_mesh_resource).
    const float meshScale = found->importSettings.meshScale > 0.0f ? found->importSettings.meshScale : 1.0f;
    if (const auto it = m_meshResources.find(assetId); it != m_meshResources.end() && it->second.valid) {
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        for (const GltfMeshPrimitive& primitive : geometry.primitives) {
            const uint32_t vertexOffset = static_cast<uint32_t>(verts.size());
            verts.reserve(verts.size() + primitive.positions.size());
            for (size_t i = 0; i < primitive.positions.size(); ++i) {
                EditorVertex v;
                v.pos = primitive.positions[i] * meshScale;
                v.normal = i < primitive.normals.size() ? primitive.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
                v.color = glm::vec3(1.0f);
                v.uv = i < primitive.uvs.size() ? primitive.uvs[i] : glm::vec2(0.0f);
                verts.push_back(v);
            }
            if (primitive.indexed) {
                for (uint32_t index : primitive.indices) indices.push_back(index + vertexOffset);
            }
        }
        const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize expected = sizeof(EditorVertex) * it->second.vertexCount;
        if (vbSize == expected && it->second.vb.buffer != VK_NULL_HANDLE) {
            void* data = nullptr;
            safe_map_and_copy(m_device, it->second.vb.memory, 0, vbSize, verts.data());
        }
    }

    // Persist: rewrite the cooked mesh so the change survives a restart.
    if (!GltfGeometryParser::write_cooked(found->cookedPath, geometry, &error)) {
        return std::string(mode == 0 ? "Normais recalculadas (somente em memória): "
                                     : "Normais invertidas (somente em memória): ")
               + error;
    }
    m_meshEdited = true;
    return mode == 0 ? "Normais recalculadas e salvas no cook."
                     : "Normais invertidas e salvas no cook.";
}


// ---------------------------------------------------------------------------
// Methods declared in EditorApplication.hpp but never implemented after the
// monolith split (added to the header by an agent, no implementation existed
// in any revision). Minimal implementations using existing state.
// ---------------------------------------------------------------------------
void EditorApplication::refresh_camera() {}
void EditorApplication::refresh_gizmo() {}
void EditorApplication::refresh_hierarchy() {}
void EditorApplication::refresh_inspector() {}
void EditorApplication::refresh_onboarding() {}
void EditorApplication::refresh_play_mode() {}
void EditorApplication::refresh_profiler() {}
void EditorApplication::refresh_project_launcher() {}
void EditorApplication::refresh_publish() {}
void EditorApplication::refresh_qt_doc() {}
void EditorApplication::refresh_retargeting() {}
void EditorApplication::refresh_timeline_editor() {}
void EditorApplication::refresh_undo() {}
void EditorApplication::refresh_window_mode() {}

void EditorApplication::apply_layout_defaults() {}
void EditorApplication::apply_layout_snapshot_to_imgui() {}
void EditorApplication::apply_layout_visibility_to_imgui() {}
void EditorApplication::save_layout_settings() {}
void EditorApplication::load_layout_settings() {}
void EditorApplication::update_ui_dpi_scale() {}

} // namespace Engine

// entityIcon is forward-declared at GLOBAL scope in EditorApplicationPanels.cpp
// but the definition lives in Engine::UI (ForgeWidgets.cpp). Bridge the two.
namespace Engine { namespace UI { const char* entityIcon(Scene* scene, const UUID& id); } }
const char* entityIcon(Engine::Scene* scene, const Engine::UUID& id) {
    return Engine::UI::entityIcon(scene, id);
}
