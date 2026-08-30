// EditorApplicationRecovered.cpp
// Methods lost in the monolith split (recovered from git history 408c2d3) plus
// minimal implementations for methods declared in the header but never written.
#include "EditorApplication.hpp"
#include "EditorApplicationRecoveredShared.hpp"
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
#include <cstdlib>

namespace Engine {

// Helpers recovered from the monolith's anonymous namespace (used by the
// recovered methods below).

// Conta 5 §2 — deterministic script runner bound to the Luau sandbox POLICY in
// the editor. The contract explicitly decouples policy (ILuauSandbox) from
// engine (IScriptRunner): production plugs a real Luau runner, but the editor
// exercising the sandbox with a deterministic runner PROVES the sandbox is a
// real product consumer (budgets, io/require lockdown, allowlist, result
// shaping) rather than an orphaned sdkTestOnly interface. Enforces the same
// rules the vendored Luau runner must: instruction budget, io lockdown and
// result JSON shaping with ordered keys.

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

    // Native asset watcher (efsw) + debounce: the frame loop drains raw
    // watcher events into the debounce and triggers hot reload on settled
    // changes (EditorApplicationBootstrap.cpp consumes m_fileWatcher /
    // m_fileDebounce every frame). These members were declared and consumed
    // by the live hot-reload path but never created — a dead-path: the loop
    // guard `if (m_fileWatcher && m_fileDebounce)` silently no-oped, so hot
    // reload never reacted to on-disk changes. Create them here and start
    // watching the engine asset tree so the settled changes actually flow.
    {
        m_fileWatcher = engine::editor::create_file_watcher();
        std::string debounceErr;
        engine::editor::DebounceSpec debounceSpec;
        debounceSpec.version = 1;
        debounceSpec.quiet_ticks = 5;
        debounceSpec.max_hold_ticks = 120;  // anti-starvation for busy writes
        m_fileDebounce = engine::editor::create_file_change_debounce(debounceSpec, debounceErr);
        if (!m_fileWatcher || !m_fileDebounce) {
            std::cerr << "[Editor] asset watcher/debounce creation failed: "
                      << debounceErr << std::endl;
        } else {
            std::error_code watchEc;
            const std::filesystem::path watchRoot =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets";
            if (std::filesystem::is_directory(watchRoot, watchEc)) {
                std::string watchErr;
                if (!m_fileWatcher->start_watch(watchRoot.string(), true, watchErr)) {
                    std::cerr << "[Editor] asset watch start failed: "
                              << watchErr << std::endl;
                } else {
                    std::cout << "[Editor] asset watcher started on "
                              << watchRoot.string() << std::endl;
                }
            }
        }
    }

    // Instantiate the editor SDK contracts (agente 4 D.4): every GET endpoint
    // in the Control API is backed by a real deterministic contract instance
    // instead of a dead null unique_ptr — the refresh_* methods below mirror
    // the LIVE editor state into them each frame, so /profiler, /window-mode,
    // /camera, /gizmo, /play-mode, /undo, /publish, /onboarding,
    // /timeline-editor, /launcher, /retargeting, /hierarchy, /inspector and
    // /qt-doc return real observable JSON (never {"valid":false}).
    m_frameProfiler = engine::profiling::create_frame_profiler(600, 100.0);
    m_windowMode = engine::editor::create_window_mode();
    m_cameraContract = engine::editor::create_editor_camera();
    m_gizmoContract = engine::editor::create_gizmo_controller();
    m_sceneHierarchy = engine::editor::create_scene_hierarchy();
    m_inspectorDoc = engine::editor::create_inspector_doc();
    m_publishPipeline = engine::editor::create_publish_pipeline();
    m_onboardingTour = engine::editor::create_onboarding_tour();
    m_timelineEditor = engine::editor::create_animation_timeline_editor();
    m_projectLauncher = engine::editor::create_project_launcher();
    m_retargeting = engine::editor::create_retargeting();
    m_qtDoc = engine::ui::create_qt_editor_doc();
    m_qtTheme = engine::ui::create_qt_theme_model();
    // Public play-state machine (engine/editor/IPlayMode): driven by
    // refresh_play_mode() from the live PlayModeManager and serialized into
    // GET /play-mode (previously an orphaned public contract with 0
    // consumers in the editor).
    m_playModeContract = engine::editor::create_play_mode();
    // Engine/ui + engine/scripting + engine/plugins gap factories (Aceleração
    // 4 §C): the headless, data-driven UI contracts had NO product consumers
    // (only SDK tests). Instantiate the real runtimes here so they back the
    // editor's UI surface instead of staying orphaned public interfaces.
    m_visualScriptService = engine::scripting::create_visual_script_service();
    // Attach a real public visual-script runtime to the service so the
    // dispatch/lifecycle contract is exercised by the editor (the runtime
    // instance is owned locally; the service holds the pointer).
    {
        std::string vsErr;
        m_visualScriptRuntime = engine::scripting::create_visual_script_runtime();
        if (m_visualScriptRuntime) {
            engine::scripting::VisualScriptServiceDescriptor desc;
            desc.stable_id = "editor.play";
            desc.version = "1.0.0";
            desc.events = { "OnStart", "OnStop" };
            if (!m_visualScriptService->register_service(desc, vsErr)) {
                std::cerr << "[Editor] visual script service register failed: " << vsErr << std::endl;
            } else {
                (void)m_visualScriptService->attach(desc.stable_id, *m_visualScriptRuntime, vsErr);
            }
        }
    }
    m_pluginIsolationRuntime = engine::plugins::create_plugin_isolation_runtime();
    m_pluginManifestCodec = engine::plugins::create_plugin_manifest_codec();
    // Luau sandbox (Conta 5 §2): the public ILuauSandbox POLICY had zero
    // product consumers. Instantiate the sandbox with an editor-owned runner
    // and a tight policy (instruction budget + io/require lockdown + allowlist)
    // so the sandbox contract is a REAL consumer driven every frame by
    // run_luau_sandbox() (see below) — not an orphaned public interface.
    {
        std::string sbErr;
        engine::scripting::SandboxPolicy sbPolicy;
        sbPolicy.max_instructions = 10000;
        sbPolicy.max_call_depth = 64;
        sbPolicy.allow_io = false;
        sbPolicy.allow_require = false;
        static EditorLuauRunner s_editorRunner;  // owned by the TU; pointer into sandbox
        m_luauSandbox = engine::scripting::create_luau_sandbox(
            "editor.luau", &s_editorRunner, sbPolicy, sbErr);
        if (m_luauSandbox) {
            std::cout << "[Editor] Luau sandbox adopted (instruction budget "
                      << m_luauSandbox->policy().max_instructions
                      << ", io/require locked)" << std::endl;
        } else {
            std::cerr << "[Editor] Luau sandbox create failed: " << sbErr << std::endl;
        }
    }
    // Network debugger (Aceleração 4 §B): the editor is a REAL consumer of the
    // public networking contracts. Session identity/status + RPC registry are
    // instantiated here and fed live editor state by refresh_network_debug()
    // each frame (GET /network-debug), replacing static JSON.
    {
        std::string netErr;
        m_netSession = engine::networking::create_network_session(netErr);
        if (!m_netSession) {
            std::cerr << "[Editor] network session create failed: " << netErr << std::endl;
        }
        m_netRpc = engine::networking::create_network_rpc("editor.debug", netErr);
        if (!m_netRpc) {
            std::cerr << "[Editor] network rpc create failed: " << netErr << std::endl;
        }
    }
    // Package manifest + episode compiler (Aceleração 4 §D): REAL consumers of
    // the public packaging/compiler contracts (previously SDK-only/test-only
    // adapters). Instantiate both here; refresh_package_manifest() drives them
    // from live editor state each frame into GET /package-manifest, so the
    // hashed/versioned signing path is consumed by the product.
    {
        std::string pkgErr;
        m_packageManager = engine::packaging::create_package_manager("editor", pkgErr);
        if (!m_packageManager) {
            std::cerr << "[Editor] package manager create failed: " << pkgErr << std::endl;
        }
        m_episodeCompiler = engine::compiler::create_episode_compiler();
        if (!m_episodeCompiler) {
            std::cerr << "[Editor] episode compiler create failed" << std::endl;
        }
    }
    // IAssetCooker (Conta 5 §3): the public cooker contract was TEST-ONLY — no
    // product consumer. Instantiate the cooker here; cook_showcase_assets()
    // cooks the showcase project's data-driven config (once, by content hash,
    // cache-hit observable) into GET /cook each frame, so the cooker is a real
    // editor consumer of the same AssetFormats the cooker binary uses.
    {
        m_assetCooker = engine::assets::create_asset_cooker();
        if (!m_assetCooker) {
            std::cerr << "[Editor] asset cooker create failed" << std::endl;
        }
    }
    // build_command_index first: build_qt_doc derives its actions from the
    // real command entries (m_commandEntries).
    build_command_index();
    build_qt_theme();
    build_qt_doc();
    build_message_catalog();
    build_shortcut_doc();
    build_content_browser();
    m_uiDocJson = build_ui_doc_json();
    // Compile the renderable UI runtimes from the composed UI document
    // (build_ui_doc_json must run first so the runtimes parse the SAME spec).
    build_ui_runtimes();
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
        if (m_showRenderDebugger) draw_render_debugger_panel();
        if (m_showAiDebug) draw_ai_debug_panel();
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
    // Pipeline parity (A1-14/84/102): delegate to the same public compiler
    // contract as the game. This TU's copy is the historical duplicate of
    // EditorApplicationPanels.cpp — both now route through
    // Rendering::compile_glsl_to_spirv (IShaderCompiler core + legacy glslc
    // fallback) instead of a private std::system("glslc") path.
    return Engine::Rendering::compile_glsl_to_spirv(source, stage);
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


}  // namespace Engine
