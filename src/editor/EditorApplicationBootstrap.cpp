#include "EditorApplication.hpp"
#include "EditorInternalHelpers.hpp"
#include "../engine/scene/SceneComponentIntegration.hpp"
#include "frontend/ForgeTheme.hpp"
#include "frontend/IconsFontAwesome6.h"
#include "frontend/FontAwesomeV6.h"
#include "frontend/liberation_sans.h"
#include "../engine/public/engine/compression/ICompressionProvider.hpp"
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

namespace Engine {
// Assinatura restaurada: o split havia cortado o cabeçalho de run()
// (run/try/init_window/init_vulkan/init_imgui) — ver git 408c2d3.
int EditorApplication::run() {
    try {
        init_window();
        init_vulkan();
        init_imgui();
        init_offscreen_target();
        init_scene_pipeline();
        init_geometry_buffers();
        init_default_scene();
        // Autosave recovery: a session that closed without an explicit save
        // leaves assets/scenes/autosave.scene — load it so nothing is lost.
        {
            const std::filesystem::path recovery =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes" / "autosave.scene";
            if (std::filesystem::is_regular_file(recovery)) {
                load_scene_file(recovery.string());
                std::cout << "[Editor] Autosave recuperado: " << recovery.string() << std::endl;
            }
        }
        // Persisted editor preferences (language, VSync, shadows, theme).
        load_settings();
        load_layout_settings();
        // Reapply a persisted theme to the live ImGui style on boot (the
        // Theme Editor panel persists bg/panel colors to settings.json; they
        // used to only be applied when the user pressed "Aplicar Tema").
        m_wickedTools.apply_theme_to_style();
        update_ui_dpi_scale();

        // VC_EDITOR_TEST_RENDERGRAPH=1: exercise the render graph executor on
        // the real device — a two-pass graph (Scene → Composite) is recorded
        // and submitted headlessly before the main loop, asserting the compiled
        // pass order and barriers drive the frame.
        if (std::getenv("VC_EDITOR_TEST_RENDERGRAPH") != nullptr) {
            std::exit(run_render_graph_self_test());
        }

        // VC_EDITOR_TEST_HDR=1: cook a tiny Radiance HDR, load it through the
        // material-graph texture path (must produce a real R16G16B16A16_SFLOAT
        // image, not the old solid fallback), read the pixel back and bind it
        // in a material-graph pipeline.
        if (std::getenv("VC_EDITOR_TEST_HDR") != nullptr) {
            std::exit(run_hdr_texture_self_test());
        }

        main_loop();
        cleanup();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[Editor Fatal Error] " << e.what() << std::endl;
        return -1;
    }
}

void EditorApplication::init_window() {
    if (!glfwInit()) throw std::runtime_error("Failed to initialize GLFW");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight, tr("VulkanCraft Engine - Gerenciador de Jogos", "VulkanCraft Engine - Game Launcher"), nullptr, nullptr);
    if (!m_window) throw std::runtime_error("Failed to create GLFW window");
    // Own scroll callback (chained by the ImGui backend, which stores it as its
    // previous callback and forwards events). The camera consumes the delta
    // when the 3D view is hovered; io.MouseWheel alone is useless there because
    // ImGui clears it at the end of NewFrame, after the camera update.
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, [](GLFWwindow* win, int width, int height) {
        if (auto* app = static_cast<EditorApplication*>(glfwGetWindowUserPointer(win))) {
            app->m_windowWidth = std::max(width, 1);
            app->m_windowHeight = std::max(height, 1);
            app->m_recreateSwapchain = true;
        }
    });
    glfwSetWindowContentScaleCallback(m_window, [](GLFWwindow* win, float, float) {
        if (auto* app = static_cast<EditorApplication*>(glfwGetWindowUserPointer(win)))
            app->m_lastAppliedDpiScale = 0.0f;
    });
    glfwSetScrollCallback(m_window, [](GLFWwindow* win, double /*xoff*/, double yoff) {
        if (auto* app = static_cast<EditorApplication*>(glfwGetWindowUserPointer(win))) {
            app->m_scrollAccum += yoff;
        }
    });
    // Seed the mouse position so the first camera frame has no fake jump.
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    m_lastMousePos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));
}

void EditorApplication::init_vulkan() {
    // VC_EDITOR_VALIDATION=1 enables the Vulkan validation layers + default
    // debug messenger (messages go to stderr). VC_EDITOR_SKIP_LAUNCHER=1 skips
    // the launcher hub so the 3D viewport is exercised immediately.
    const bool validate = std::getenv("VC_EDITOR_VALIDATION") != nullptr;
    vkb::InstanceBuilder builder;
    builder.set_app_name("VulkanCraft Engine")
           .require_api_version(1, 3, 0);
    if (validate) {
        builder.request_validation_layers(true);
        builder.use_default_debug_messenger();
    } else {
        builder.request_validation_layers(false);
    }
    auto inst_ret = builder.build();
    if (!inst_ret) throw std::runtime_error("Failed to create Vulkan instance");
    vkb::Instance vkb_inst = inst_ret.value();
    m_instance = vkb_inst.instance;

    glfwCreateWindowSurface(m_instance, m_window, nullptr, &m_surface);

    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    auto phys_ret = selector.set_surface(m_surface)
                            .set_minimum_version(1, 3)
                            .select();
    if (!phys_ret) throw std::runtime_error("Failed to select physical GPU");
    vkb::PhysicalDevice vkb_gpu = phys_ret.value();
    m_physicalDevice = vkb_gpu.physical_device;
    VkPhysicalDeviceProperties deviceProps{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &deviceProps);
    m_gpuName = deviceProps.deviceName ? deviceProps.deviceName : "Unknown GPU";

    vkb::DeviceBuilder device_builder{ vkb_gpu };
    auto dev_ret = device_builder.build();
    if (!dev_ret) throw std::runtime_error("Failed to create logical device");
    vkb::Device vkb_dev = dev_ret.value();
    m_device = vkb_dev.device;
    m_graphicsQueue = vkb_dev.get_queue(vkb::QueueType::graphics).value();
    m_graphicsQueueFamily = vkb_dev.get_queue_index(vkb::QueueType::graphics).value();

    // Swapchain creation with UNORM format (prevents washed-out sRGB gamma colors)
    vkb::SwapchainBuilder swapchain_builder{ vkb_dev };
    auto swap_ret = swapchain_builder
        .set_desired_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        .set_desired_extent(m_windowWidth, m_windowHeight)
        .build();
    if (!swap_ret) throw std::runtime_error("Failed to build swapchain");
    vkb::Swapchain vkb_swap = swap_ret.value();
    m_swapchain = vkb_swap.swapchain;
    m_swapchainFormat = vkb_swap.image_format;
    m_swapchainExtent = vkb_swap.extent;
    m_swapchainImages = vkb_swap.get_images().value();
    m_swapchainViews = vkb_swap.get_image_views().value();

    // Render pass creation
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pass");
    }

    // Framebuffers
    m_framebuffers.resize(m_swapchainViews.size());
    for (size_t i = 0; i < m_swapchainViews.size(); i++) {
        VkImageView attachments[] = { m_swapchainViews[i] };
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = m_swapchainExtent.width;
        framebufferInfo.height = m_swapchainExtent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create framebuffer");
        }
    }

    // Command pool and buffers
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }

    m_commandBuffers.resize(2);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 2;
    if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }

    // Synchronization primitives
    m_imageAvailableSemaphores.resize(2);
    m_renderFinishedSemaphores.resize(2);
    m_inFlightFences.resize(2);

    VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };

    for (size_t i = 0; i < 2; i++) {
        vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]);
        vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]);
        vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]);
    }
}

void EditorApplication::init_imgui() {
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * 11;
    pool_info.poolSizeCount = static_cast<uint32_t>(sizeof(pool_sizes)/sizeof(pool_sizes[0]));
    pool_info.pPoolSizes = pool_sizes;
    vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_imguiDescriptorPool);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "imgui.ini";
    io.FontGlobalScale = 1.0f; // Forge design system: roomy, not oversized

    // Frontend port (Wicked Editor, MIT): the base UI font is Liberation Sans
    // (the same font the Wicked editor ships, embedded zstd-compressed here),
    // decompressed through the public compression provider. Static storage
    // keeps the TTF alive for the atlas; the atlas must NOT take ownership.
    {
        auto provider = ::engine::compression::create_zstd_compression_provider();
        std::string uiFontData = provider->decompress(std::string(
            reinterpret_cast<const char*>(liberation_sans_zstd), sizeof(liberation_sans_zstd)));
        if (!uiFontData.empty()) {
            static std::string s_uiFont = std::move(uiFontData);
            ImFontConfig baseConfig{};
            baseConfig.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(const_cast<char*>(s_uiFont.data()),
                                           static_cast<int>(s_uiFont.size()), 15.0f,
                                           &baseConfig, io.Fonts->GetGlyphRangesDefault());
        } else {
            io.Fonts->AddFontDefault();
        }
    }
    // Merge the Font Awesome 6 Solid icon font into the base font so ICON_FA_*
    // strings render inline. Glyph range from IconsFontAwesome6.h (0xe005–0xf8ff).
    static const ImWchar s_iconRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconConfig{};
    iconConfig.MergeMode = true;
    iconConfig.GlyphMinAdvanceX = 16.0f;
    iconConfig.GlyphOffset = ImVec2(0.0f, 1.0f);
    iconConfig.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(font_awesome_v6),
                                   static_cast<int>(sizeof(font_awesome_v6)), 15.0f,
                                   &iconConfig, s_iconRanges);

    // Forge design system (light, product-grade). WindowMinSize (no panel can
    // shrink below this) is set inside applyForgeTheme.
    UI::applyForgeTheme();

    ImGui_ImplGlfw_InitForVulkan(m_window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = m_instance;
    init_info.PhysicalDevice = m_physicalDevice;
    init_info.Device = m_device;
    init_info.QueueFamily = m_graphicsQueueFamily;
    init_info.Queue = m_graphicsQueue;
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = m_imguiDescriptorPool;
    init_info.MinImageCount = 2;
    init_info.ImageCount = 2;
    init_info.PipelineInfoMain.RenderPass = m_renderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&init_info);

    // Diagnostic (post-atlas-build): which console glyphs are missing?
    {
        const char* probe = "Português (Brasil) | Memória RAM | Placa de Vídeo | çãõéêáíóúâôàü";
        std::string missing;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(probe); *p; ++p) {
            if (*p < 0x20) continue;
            ImFont* font = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
            if (!font || !font->IsGlyphInFont(*p)) {
                char b[16];
                snprintf(b, sizeof(b), "U+%04X ", *p);
                missing += b;
            }
        }
        std::cout << "[Font] " << (missing.empty() ? "All console glyphs covered" : ("Missing glyphs: " + missing)) << std::endl;
    }
}

void EditorApplication::init_default_scene() {
    // AGENTE 2 block B: register the builtin component descriptors through the
    // Scene<->ECS integration layer (SceneComponentIntegration was an orphaned
    // adapter with zero consumers). This makes the editor's component surface
    // (typed + generic reflection) available to serialization, undo/redo,
    // plugin components and network replication — the same component identity
    // the SDK/MCP-authored components use.
    SceneComponentIntegration::register_builtin_components();
    m_editorScene = std::make_unique<Scene>("Untitled Scene");
    m_playMode.set_editor_scene(m_editorScene.get());
    m_activeScenePath.clear();
    m_autosavePath.clear();
    m_sceneDirty = false;
    // A fresh scene has no authored terrain (the previous scene's heightmap
    // must not leak into the new one).
    clear_terrain_mesh();

    Entity camera = m_editorScene->create_entity(tr("Câmera Principal", "Main Camera"));
    m_editorScene->transformComponents[camera.get_id()].position = glm::vec3(0.0f, 2.0f, 5.0f);
    m_editorScene->cameraComponents[camera.get_id()] = CameraComponent{ 70.0f, 0.1f, 2000.0f, true };

    Entity sun = m_editorScene->create_entity(tr("Luz Direcional", "Directional Light"));
    m_editorScene->lightComponents[sun.get_id()] = LightComponent{ glm::vec3(1.0f, 0.95f, 0.85f), 10000.0f, 1000.0f, true };
    m_editorScene->transformComponents[sun.get_id()].rotation = glm::vec3(-45.0f, 30.0f, 0.0f);

    m_selectedEntity = camera;
    m_editorGui.init(m_editorScene.get(), &m_undo);
    m_editorGui.set_asset_registry(&m_assetRegistry);
    m_editorGui.select_entity(m_selectedEntity);
    // Disable the EditorGUI's own (English, undocked) panels: the real panels
    // are the Portuguese ones drawn by EditorApplication. Keeping the flags
    // false prevents duplicate floating windows ("World Outliner", "Inspector"...).
    m_editorGui.showOutliner = false;
    m_editorGui.showInspector = false;
    m_editorGui.showContentBrowser = false;
    m_editorGui.showConsole = false;
    m_editorGui.showVoxelTools = false;
    m_editorGui.showProfiler = false;

    if (std::getenv("VC_EDITOR_SKIP_LAUNCHER") != nullptr) {
        m_inLauncherMode = false;
        glfwSetWindowTitle(m_window, ("VulkanCraft Engine - [" + m_currentProjectName + "]").c_str());
    }

    // VC_EDITOR_TEST_MATERIAL=1: exercise the material-graph viewport path
    // (graph → GLSL → glslc → pipeline → per-entity UBO → draw) headlessly.
    if (std::getenv("VC_EDITOR_TEST_MATERIAL") != nullptr) {
        // The material graph pipeline is built by the viewport pass, which
        // only runs outside the launcher hub — leave the hub for this test.
        m_inLauncherMode = false;
        m_materialTestMatId = UUID();
        m_materialTestMeshId = UUID();
        Entity matCube = m_editorScene->create_entity("Material Test Cube");
        m_editorScene->transformComponents[matCube.get_id()].position = glm::vec3(2.0f, 1.0f, 0.0f);
        m_editorScene->materialComponents[matCube.get_id()] = MaterialComponent{
            glm::vec3(0.9f, 0.25f, 0.15f), 0.35f, 0.1f, glm::vec3(0.0f), 0.0f };
        m_editorScene->meshRendererComponents[matCube.get_id()] =
            MeshRendererComponent{ m_materialTestMeshId, m_materialTestMatId, true, true };
        // GPU mesh resource built from cube geometry (no asset round-trip).
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        generate_cube_geometry(verts, indices);
        EditorMeshResource cubeRes;
        cubeRes.vertexCount = static_cast<uint32_t>(verts.size());
        cubeRes.ranges.push_back({ 0, static_cast<uint32_t>(indices.size()), 0, true });
        const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
        create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      cubeRes.vb.buffer, cubeRes.vb.memory);
        create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      cubeRes.ib.buffer, cubeRes.ib.memory);
        safe_map_and_copy(m_device, cubeRes.vb.memory, 0, vbSize, verts.data());
        safe_map_and_copy(m_device, cubeRes.ib.memory, 0, ibSize, indices.data());
        cubeRes.valid = true;
        m_meshResources[m_materialTestMeshId] = std::move(cubeRes);
        // Material asset values become the graph's parameter defaults.
        MaterialAsset testMat;
        testMat.albedo = glm::vec3(0.2f, 0.6f, 0.9f);
        testMat.roughness = 0.7f;
        testMat.metallic = 0.0f;
        m_materialAssets[m_materialTestMatId] = testMat;
        m_materialTestFramesLeft = 150;
    }

    // VC_EDITOR_TEST_PLAY=1: a cube with a rigidbody is dropped into the play
    // world; the test asserts gravity moved it down while the viewport renders
    // the play scene.
    if (std::getenv("VC_EDITOR_TEST_PLAY") != nullptr) {
        // The play world only ticks outside the launcher hub (main_loop gates
        // tick_play_runtime on !m_inLauncherMode), so leave the hub for this
        // headless verification of the in-engine game.
        m_inLauncherMode = false;
        Entity fallingCube = m_editorScene->create_entity("Falling Cube");
        m_editorScene->transformComponents[fallingCube.get_id()].position = glm::vec3(0.0f, 5.0f, 0.0f);
        m_editorScene->rigidbodyComponents[fallingCube.get_id()] =
            RigidbodyComponent{ 1.0f, 0.5f, 0.1f, false, true };
        m_playTestEntityId = fallingCube.get_id();
        m_playMode.start_play(m_editorScene.get());
        setup_play_runtime();
        m_playTestFramesLeft = 120;
    }

    // VC_EDITOR_TEST_BUILD=1: run the full Build Game pipeline headlessly and
    // exit with the build result. A visible mesh entity is added first so the
    // packaged initial scene actually renders something.
    if (std::getenv("VC_EDITOR_TEST_BUILD") != nullptr) {
        Entity buildCube = m_editorScene->create_entity("Build Cube");
        m_editorScene->transformComponents[buildCube.get_id()].position = glm::vec3(0.0f, 0.5f, 0.0f);
        m_editorScene->materialComponents[buildCube.get_id()] = MaterialComponent{
            glm::vec3(0.25f, 0.65f, 0.90f), 0.35f, 0.1f, glm::vec3(0.0f), 0.0f };
        m_editorScene->meshRendererComponents[buildCube.get_id()] = MeshRendererComponent{};
        m_editorScene->rigidbodyComponents[buildCube.get_id()] = RigidbodyComponent{};
        run_game_build();
        const bool buildOk = std::none_of(m_buildLog.begin(), m_buildLog.end(),
                                          [](const std::string& line) { return line.rfind("Build failed", 0) == 0; });
        std::cout << "[Editor] BUILD_TEST " << (buildOk ? "PASS" : "FAIL") << std::endl;
        std::exit(buildOk ? 0 : 1);
    }
}


// ===========================================================================
// Core (constructor, init, main loop, render frame). Monolithic: a split
// attempt (EditorApplication_{Panels,Vulkan,PlayMode,Assets}.cpp, 18/ago) was
// never wired into CMake — those orphan duplicates were removed (2026-08-26,
// AGENT-2 §B dedup). All editor code lives here.
// ===========================================================================

int EditorApplication::run_render_graph_self_test() {
    using namespace Engine::Rendering;

    // Match the offscreen viewport target to the swapchain so a single render
    // area extent serves both passes.
    recreate_offscreen_if_needed(m_swapchainExtent.width, m_swapchainExtent.height);
    if (m_offscreen.framebuffer == VK_NULL_HANDLE) {
        std::cerr << "[Editor] RENDERGRAPH_TEST FAIL (no offscreen framebuffer)" << std::endl;
        return 1;
    }

    // Two-pass graph: Scene writes the offscreen color+depth; Composite reads
    // the color and writes the swapchain.
    RenderGraph graph;
    const auto sceneColor = graph.add_resource({ "Scene Color", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    const auto sceneDepth = graph.add_resource({ "Scene Depth", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    const auto swap = graph.add_resource({ "Swapchain", RenderResourceKind::Image, 0,
        m_swapchainExtent.width, m_swapchainExtent.height, 1, false, true, RenderResourceState::Present });
    const auto scenePass = graph.add_pass({ "Scene", RenderQueue::Graphics,
        { { sceneColor, RenderAccess::Write, RenderResourceState::ColorAttachment },
          { sceneDepth, RenderAccess::Write, RenderResourceState::DepthAttachment } }, true });
    const auto compositePass = graph.add_pass({ "Composite", RenderQueue::Graphics,
        { { sceneColor, RenderAccess::Read, RenderResourceState::ShaderRead },
          { swap, RenderAccess::Write, RenderResourceState::Present } }, true });
    (void)graph.add_dependency(scenePass, compositePass);

    VulkanRenderGraphExecutor executor;
    std::string error;
    if (!executor.initialize(m_device, graph, &error)) {
        std::cerr << "[Editor] RENDERGRAPH_TEST FAIL (init: " << error << ")" << std::endl;
        return 1;
    }
    const RenderGraphCompileResult& compiled = executor.compile_result();
    if (compiled.order.size() != 2 || compiled.barriers.empty()) {
        std::cerr << "[Editor] RENDERGRAPH_TEST FAIL (order=" << compiled.order.size()
                  << ", barriers=" << compiled.barriers.size() << ")" << std::endl;
        return 1;
    }

    // The executor begins each pass itself, so the draw callbacks must record
    // content only (no render-pass begin) — here both passes are clear-only.
    // The game runtime (main_game.cpp) drives real content through these
    // callbacks (drawScene/drawComposite).
    VulkanRenderGraphExecutor::PassFrame sceneFrame;
    sceneFrame.renderPass = m_offscreen.renderPass;
    sceneFrame.framebuffers = { m_offscreen.framebuffer };
    sceneFrame.clearValues.resize(2);
    sceneFrame.clearValues[0].color = { { 0.2f, 0.3f, 0.4f, 1.0f } };
    sceneFrame.clearValues[1].depthStencil = { 1.0f, 0 };
    sceneFrame.draw = [](VkCommandBuffer /*cb*/) {};
    executor.register_pass(scenePass, std::move(sceneFrame));

    VulkanRenderGraphExecutor::PassFrame compositeFrame;
    compositeFrame.renderPass = m_renderPass;
    compositeFrame.framebuffers = m_framebuffers;
    compositeFrame.clearValues.resize(1);
    compositeFrame.clearValues[0].color = { { 0.08f, 0.09f, 0.12f, 1.0f } };
    compositeFrame.draw = [](VkCommandBuffer /*cb*/) {};
    executor.register_pass(compositePass, std::move(compositeFrame));

    // Record one full frame through the executor and submit it.
    VkCommandBuffer cb = m_commandBuffers[0];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &begin);
    executor.record(cb, 0, m_swapchainExtent);
    vkEndCommandBuffer(cb);
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    const VkResult result = vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkDeviceWaitIdle(m_device);
    vkQueueWaitIdle(m_graphicsQueue);

    const bool ok = result == VK_SUCCESS && executor.executed_pass_count() == 2 &&
                    executor.total_barriers() >= 1;
    std::cout << "[Editor] RENDERGRAPH_TEST " << (ok ? "PASS" : "FAIL")
              << " (passes=" << executor.executed_pass_count()
              << ", barriers=" << executor.total_barriers() << ")" << std::endl;
    return ok ? 0 : 1;
}

// VC_EDITOR_TEST_HDR=1: cooks a tiny Radiance HDR (2x1, left pixel red = 4.0),
// loads it through load_viewport_texture — which must produce a real
// R16G16B16A16_SFLOAT image instead of the old flat-shading fallback — then
// reads the first pixel back (half 0x4800 = 4.0) and builds a material-graph
// pipeline bound to the HDR texture.
int EditorApplication::run_hdr_texture_self_test() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path src = dir / "vc_hdr_selftest.hdr";
    const std::filesystem::path cookedDir = dir / "vc_hdr_selftest_cooked";
    std::ofstream out(src, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[Editor] HDR_TEST FAIL (cannot write " << src << ")" << std::endl;
        return 1;
    }
    // Radiance RGBE header; orientation line "-Y 1 +X 2" => 2x1, top-left first.
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    // Pixel 0: RGBE {1,0,0,138} => r = 2^(138-136) = 4.0 (half 0x4400).
    // Pixel 1: black.
    const uint8_t pixels[8] = { 1, 0, 0, 138, 0, 0, 0, 0 };
    out.write(reinterpret_cast<const char*>(pixels), 8);
    out.close();

    ImportRequest request;
    request.source = src;
    request.cookedDirectory = cookedDir;
    const ImportResult cooked = m_assetPipeline->import(request);
    if (!cooked) {
        std::cerr << "[Editor] HDR_TEST FAIL (cook: " << cooked.error << ")" << std::endl;
        return 1;
    }

    GraphTexture tex;
    std::string error;
    if (!load_viewport_texture(cooked.asset.id, tex, error)) {
        std::cerr << "[Editor] HDR_TEST FAIL (load: " << error << ")" << std::endl;
        return 1;
    }
    if (tex.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
        std::cerr << "[Editor] HDR_TEST FAIL (format " << static_cast<int>(tex.format)
                  << " != R16G16B16A16_SFLOAT)" << std::endl;
        destroy_graph_texture(tex);
        return 1;
    }

    // Read pixel 0 back and verify the half-float value survived (r = 4.0).
    const VkDeviceSize size = static_cast<VkDeviceSize>(2) * 1 * 8;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) {
        std::cerr << "[Editor] HDR_TEST FAIL (staging alloc)" << std::endl;
        destroy_graph_texture(tex);
        return 1;
    }
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, tex.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { 2, 1, 1 };
    vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
    transition_image_layout(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);
    void* data = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, size, 0, &data);
    if (!data) {
        std::cerr << "[Editor] HDR_TEST FAIL (staging map)" << std::endl;
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        destroy_graph_texture(tex);
        return 1;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    const uint16_t halfR = static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
    vkUnmapMemory(m_device, stagingMemory);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    if (halfR != 0x4400) {
        std::cerr << "[Editor] HDR_TEST FAIL (pixel r half = 0x" << std::hex << halfR
                  << ", expected 0x4400 = 4.0)" << std::dec << std::endl;
        destroy_graph_texture(tex);
        return 1;
    }

    // Build a material-graph pipeline with the HDR texture bound to BaseColor
    // (reconnect the BaseColor output of the standard PBR graph).
    Rendering::MaterialGraph graph = Rendering::material_graph_from_pbr(MaterialAsset{});
    const auto texNode = graph.add_texture_sample("HDR Texture");
    if (auto* node = graph.find_node(texNode)) node->value = cooked.asset.id.to_string();
    bool connected = false;
    for (const auto& candidate : graph.nodes()) {
        if (candidate.kind != Rendering::MaterialNodeKind::Output ||
            candidate.parameter != "BaseColor")
            continue;
        connected = graph.connect(texNode, candidate.id, 0);
        break;
    }
    GraphMaterialPipeline pipeline;
    if (!connected || !build_graph_pipeline(graph, pipeline)) {
        std::cerr << "[Editor] HDR_TEST FAIL (pipeline: "
                  << (pipeline.lastError.empty() ? "texture not connected" : pipeline.lastError) << ")" << std::endl;
        destroy_graph_texture(tex);
        return 1;
    }
    destroy_graph_pipeline(pipeline);
    destroy_graph_texture(tex);
    std::cout << "[Editor] HDR_TEST PASS (RGBA16F, pixel r=4.0 half=0x4400, texture pipeline bound)" << std::endl;
    return 0;
}

void EditorApplication::main_loop() {
    double lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();
        update_ui_dpi_scale();

        double currentTime = glfwGetTime();
        float deltaTime = static_cast<float>(currentTime - lastTime);
        lastTime = currentTime;
        m_fps = 1.0f / std::max(deltaTime, 0.001f);
        m_frameTimeMs = deltaTime * 1000.0f;
        if (m_frameProfiler) {
            m_frameProfiler->record(m_frameTimeMs,
                                    static_cast<double>(m_ramUsageMb));
        }

        // Graphics changes (Opções Gráficas) are deferred to here — before
        // acquire, with nothing in flight — so the swapchain and shadow map
        // can be recreated safely.
        if (m_recreateSwapchain) {
            m_recreateSwapchain = false;
            recreate_swapchain();
        }
        if (m_recreateShadowMap) {
            m_recreateShadowMap = false;
            const uint32_t newSize = shadow_size_from_quality(m_shadowQuality);
            if (m_shadowMap.size != newSize) {
                vkDeviceWaitIdle(m_device);
                m_shadowMap.size = newSize;
                create_shadow_map();
            }
        }

        // Control API: execute queued commands (play/pause/resume/stop/step)
        // from the loopback HTTP server. The drain runs even while the
        // launcher hub is open so API-driven actions (open-scene, new-scene,
        // create-project) can leave the hub; then publish live state for
        // /state.
        {
            for (const auto& pc : m_controlApi.drain_commands()) {
                std::cout << "[API-DEBUG] " << pc.cmd << std::endl;
                m_controlResult.clear();
                m_controlData.clear();
                handle_control_command(pc.cmd);
                m_controlApi.complete_command(pc.id, m_controlResult.empty(), m_controlResult, m_controlData);
            }
        }

        // Native asset watcher -> debounce -> hot reload: efsw raw events are
        // coalesced per path; once settled (quiet window elapsed), trigger the
        // same reimport path the hot-reload command uses. The polling
        // AssetHotReloadService still runs below (it is the reimporter); the
        // watcher is the event source that makes reloads prompt.
        if (m_fileWatcher && m_fileDebounce) {
            const auto rawEvents = m_fileWatcher->poll_events();
            if (!rawEvents.empty()) {
                ++m_fileDebounceTick;
                for (const auto& e : rawEvents) {
                    m_fileDebounce->record(engine::editor::FileChangeEvent{
                        e.path, e.kind, m_fileDebounceTick + e.tick });
                }
            }
            m_fileDebounceTick += 1;
            const auto settled = m_fileDebounce->advance(m_fileDebounceTick);
            if (!settled.empty() && m_assetHotReload) {
                // Rate-limit: a burst of raw events coalesces into a few
                // settled changes; each trigger re-walks every registered
                // asset (last_write_time) — once per second is plenty for
                // hot reload and keeps the loop cheap.
                const auto now = std::chrono::steady_clock::now();
                if (m_lastWatcherReload.time_since_epoch().count() == 0 ||
                    now - m_lastWatcherReload >= std::chrono::seconds(1)) {
                    m_lastWatcherReload = now;
                    m_assetHotReload->watch_registered_assets();
                    m_assetHotReload->poll();
                }
            }
        }

        if (!m_inLauncherMode) {
            {
                EditorApiState api;
                switch (m_playMode.get_state()) {
                    case PlayState::Play: api.state = "play"; break;
                    case PlayState::Pause: api.state = "pause"; break;
                    case PlayState::Simulate: api.state = "simulate"; break;
                    default: break;
                }
                Scene* s = m_playMode.get_active_scene();
                // get_active_scene() can return null in Edit mode before the
                // first Play (the cached editor-scene pointer is only set by
                // start_play / set_editor_scene); fall back to the editor's
                // own scene, matching every render/tick path.
                if (!s) s = m_editorScene.get();
                api.fps = m_fps;
                api.entities = s ? s->get_entities().size() : 0u;
                api.orbitDistance = m_editorCamera.orbitDistance;
                api.viewportHovered = m_viewportHovered;
                api.imageHovered = m_viewportImageHovered;
                ImGuiIO& io = ImGui::GetIO();
                api.keyboardCapture = io.WantCaptureKeyboard;
                api.typing = io.WantTextInput;
                api.camX = m_editorCamera.position.x;
                api.camY = m_editorCamera.position.y;
                api.camZ = m_editorCamera.position.z;
                api.yaw = m_editorCamera.yaw;
                api.pitch = m_editorCamera.pitch;
                api.vsync = m_vsyncEnabled;
                api.shadowQuality = m_shadowQuality;
                api.terrainValid = m_terrainValid;
                if (m_terrainValid) {
                    const int seg = m_terrainParams.segments;
                    api.terrainVertices = static_cast<std::size_t>(seg + 1) * (seg + 1);
                    api.terrainTriangles = m_terrainIndexCount / 3;
                }
                api.meshEdited = m_meshEdited;
                api.settingsPath = m_settingsPath;
                api.selectedEntity = m_selectedEntity.is_valid()
                    ? m_selectedEntity.get_id().to_string() : std::string();
                switch (m_gizmoMode) {
                    case GizmoMode::Select: api.gizmoMode = "select"; break;
                    case GizmoMode::Translate: api.gizmoMode = "translate"; break;
                    case GizmoMode::Rotate: api.gizmoMode = "rotate"; break;
                    case GizmoMode::Scale: api.gizmoMode = "scale"; break;
                }
                if (m_gizmoLocal) api.gizmoMode += ":local";
                api.snap = m_snapTranslate;
                api.grid = m_showGrid;
                api.gizmos = m_showGizmos;
                api.colliders = m_showColliders;
                api.selectedDebugOverlay = m_selectedDebugOverlay;
                api.camTargetX = m_editorCamera.orbitTarget.x;
                api.camTargetY = m_editorCamera.orbitTarget.y;
                api.camTargetZ = m_editorCamera.orbitTarget.z;
                api.lastSelfTest = m_lastSelfTestResult;
                api.panels = m_panelRegistry.panel_ids();
                api.templates = m_templateRegistry.template_ids();
                api.ui_doc = m_uiDocJson;
                api.layout = m_layoutModel.snapshot().to_json();
                api.messages = m_messageCatalogJson;
                api.shortcuts = m_shortcutDocMarkdown;
                refresh_play_mode();
                api.play_mode = m_playModeJson;
                // Feed the headless UI runtimes (inventory grid, crafting,
                // confirmation) from the live editor state each frame.
                refresh_ui_runtimes();
                api.command_index = m_commandIndexJson;
                api.inventory_grid = m_inventoryGridJson;
                refresh_network_debug();
                api.network_debug = m_networkDebugJson;
                refresh_package_manifest();
                api.package_manifest = m_packageManifestJson;
                api.cooked_assets_json = m_cookAssetsJson;
                refresh_visual_script_lifecycle();
                run_luau_sandbox();
                cook_showcase_assets();
                // Conta 5 fechamento_global: drive the previously TEST-ONLY
                // SDK factories (jobs, procgen jobs + cancellable token +
                // preview, farm cooker, hilbert cell index [plain + JSON],
                // block-entity scripting and audio mixer) as real editor
                // consumers every frame and publish the observable.
                refresh_sdk_contract_runtimes();
                api.sdk_contracts = m_sdkContractJson;
                refresh_profiler();
                api.profiler = m_profilerJson;
                refresh_undo();
                api.undo = m_undoJson;
                api.content_browser = m_contentBrowserJson;
                refresh_window_mode();
                api.window_mode = m_windowModeJson;
                refresh_camera();
                api.camera = m_cameraJson;
                refresh_gizmo();
                api.gizmo = m_gizmoJson;
                refresh_publish();
                api.publish = m_publishJson;
                refresh_inspector();
                api.inspector = m_inspectorJson;
                refresh_hierarchy();
                api.hierarchy = m_hierarchyJson;
                refresh_onboarding();
                api.onboarding = m_onboardingJson;
                refresh_timeline_editor();
                api.timeline_editor = m_timelineEditorJson;
                refresh_project_launcher();
                api.launcher = m_projectLauncherJson;
                refresh_retargeting();
                api.retargeting = m_retargetingJson;
                refresh_render_diagnostics();
                api.render_diagnostics = m_renderDiagnosticsJson;
                refresh_qt_doc();
                api.qt_doc = m_qtDocJson;
                api.qt_theme = m_qtThemeJson;
                switch (m_playScript.status()) {
                    case VMStatus::Idle: api.scriptState = "idle"; break;
                    case VMStatus::Running: api.scriptState = "running"; break;
                    case VMStatus::Waiting: api.scriptState = "waiting"; break;
                    case VMStatus::Paused: api.scriptState = "paused"; break;
                    case VMStatus::Completed: api.scriptState = "completed"; break;
                    case VMStatus::Error: api.scriptState = "error"; break;
                }
                // Conta 5 §2: append the Luau sandbox consumer's live state
                // (a real product consumer, driven each frame by run_luau_sandbox).
                api.scriptState += std::string("|luau:") +
                    (m_luauSandbox ? "sandboxed" : "none") + ":" +
                    std::to_string(m_luauSandboxExecutions) +
                    (m_luauSandboxIoLocked ? ":io-locked" : "");
                m_controlApi.publish_state(api);
            }
            update_editor_camera(deltaTime);
            process_viewport_input();
            if (m_stepRequested && m_playMode.get_state() == PlayState::Pause) {
                // Advance the play world a single frame (Pause → Play → tick →
                // Pause) so the PASSO button works.
                m_stepRequested = false;
                m_playMode.set_state(PlayState::Play);
                tick_play_runtime(deltaTime);
                m_playMode.set_state(PlayState::Pause);
            } else {
                m_stepRequested = false;
            }
            tick_play_runtime(deltaTime);
        }

        // Audio preview: pick up background decodes and start the requested
        // voice. Must run every frame in EVERY mode (the asset browser is used
        // in edit mode, where tick_play_runtime early-returns). The miniaudio
        // device drives the mixer in real time when available; without a
        // device we still advance it so voice state (▶/⏸) stays truthful.
        pump_audio_preview_decodes();
        pump_asset_thumbnail_decodes();
        if (!m_audioDeviceStarted) m_playAudio.render(1024);

        // Scene autosave: debounced persist of every change (see
        // mark_scene_dirty / autosave_scene). Runs in every mode so API
        // mutations made before the launcher hub is left still get saved.
        autosave_scene();

        // 3D asset thumbnails (mesh + block cubes): a few renders per frame.
        pump_asset_thumbnails(4);

        // Voxel block pipelines are built here, outside the render pass:
        // creating pipelines/uploading atlases while record_viewport_scene
        // content is recording the viewport pass hung the GPU (device lost).
        ensure_voxel_pipelines();

        // Runtime-wired Wicked-port features (hair strands, soft-body cloth,
        // video flipbooks, gaussian splats, env-probe captures): preview in
        // Edit and keep simulating in Play (the same scene the viewport draws).
        Scene* simScene = m_playMode.get_active_scene();
        if (!simScene) simScene = m_editorScene.get();
        tick_special_runtimes(simScene, deltaTime);

        render_frame();

        if (m_playTestFramesLeft > 0 && --m_playTestFramesLeft == 0) {
            Scene* playScene = m_playMode.get_active_scene();
            bool fell = false;
            float y = -1.0f;
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
    }
}

} // namespace Engine
