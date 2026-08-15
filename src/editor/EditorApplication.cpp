#include "EditorApplication.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include "../engine/animation/AnimationAssets.hpp"
#include "../engine/rendering/vulkan/MaterialPipeline.hpp"
#include "../engine/audio/AudioRuntime.hpp"
#include "../engine/audio/OggDecoder.hpp"
#include "../engine/gameplay/DialogueSystem.hpp"
#include "../engine/gameplay/DestructionRuntime.hpp"
#include "../engine/gameplay/MissionSystem.hpp"
#include "../engine/navigation/Navigation.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include <unordered_set>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <fstream>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <VkBootstrap.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <wincodec.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static HANDLE g_hGameProcess = NULL;
static DWORD g_gameProcessId = 0;
static HWND g_hGameWindow = NULL;

namespace {
struct GameWindowSearch {
    DWORD processId{ 0 };
    HWND window{ NULL };
};

BOOL CALLBACK find_game_window(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<GameWindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId || GetWindow(window, GW_OWNER) != NULL ||
        !IsWindowVisible(window)) return TRUE;
    search->window = window;
    return FALSE;
}

void stop_external_game() {
    if (g_hGameWindow != NULL && IsWindow(g_hGameWindow)) SetParent(g_hGameWindow, NULL);
    g_hGameWindow = NULL;
    g_gameProcessId = 0;
    if (g_hGameProcess == NULL) return;
    DWORD exitCode = 0;
    if (GetExitCodeProcess(g_hGameProcess, &exitCode) && exitCode == STILL_ACTIVE)
        TerminateProcess(g_hGameProcess, 0);
    CloseHandle(g_hGameProcess);
    g_hGameProcess = NULL;
}

bool launch_external_game() {
    stop_external_game();
    const std::filesystem::path executable =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR).parent_path() /
        "1.5/build/Release/vulkan_craft.exe";
    if (!std::filesystem::exists(executable)) {
        std::cerr << "[Editor] Embedded game executable not found: "
                  << executable.string() << '\n';
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring application = executable.wstring();
    std::wstring workingDirectory = executable.parent_path().wstring();
    if (!CreateProcessW(application.c_str(), nullptr, nullptr, nullptr, FALSE, 0,
                        nullptr, workingDirectory.c_str(), &startup, &process)) {
        std::cerr << "[Editor] Failed to launch embedded game (Win32 error "
                  << GetLastError() << ")\n";
        return false;
    }
    g_hGameProcess = process.hProcess;
    g_gameProcessId = process.dwProcessId;
    CloseHandle(process.hThread);
    std::cout << "[Editor] Game 1.5 launched for embedded viewport preview\n";
    return true;
}

void update_embedded_game(GLFWwindow* editorWindow, const ImVec2& screenPosition,
                          const ImVec2& size) {
    if (g_hGameProcess == NULL || editorWindow == nullptr) return;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(g_hGameProcess, &exitCode) || exitCode != STILL_ACTIVE) {
        stop_external_game();
        return;
    }
    if (g_hGameWindow == NULL || !IsWindow(g_hGameWindow)) {
        GameWindowSearch search{ g_gameProcessId, NULL };
        EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&search));
        g_hGameWindow = search.window;
        if (g_hGameWindow == NULL) return;

        HWND editorNative = glfwGetWin32Window(editorWindow);
        SetParent(g_hGameWindow, editorNative);
        LONG_PTR style = GetWindowLongPtrW(g_hGameWindow, GWL_STYLE);
        style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                   WS_MAXIMIZEBOX | WS_SYSMENU);
        style |= WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        SetWindowLongPtrW(g_hGameWindow, GWL_STYLE, style);
        SetWindowPos(g_hGameWindow, HWND_TOP, 0, 0, 1, 1,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        std::cout << "[Editor] Game 1.5 attached to Scene Viewport\n";
    }

    // ImGui coordinates in this single-platform-window editor are already in
    // the GLFW client coordinate space. Calling ScreenToClient here caused the
    // editor window position to be subtracted a second time, making the child
    // game appear pinned to the desktop when the editor was dragged.
    const int clientX = static_cast<int>(std::lround(screenPosition.x));
    const int clientY = static_cast<int>(std::lround(screenPosition.y));
    MoveWindow(g_hGameWindow, clientX, clientY,
               std::max(1, static_cast<int>(std::lround(size.x))),
               std::max(1, static_cast<int>(std::lround(size.y))), TRUE);
}
} // namespace
#endif

namespace Engine {

EditorApplication::EditorApplication() {
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

EditorApplication::~EditorApplication() {
    cleanup();
}

// ---------------------------------------------------------------------------
// Editor camera (orbit around a target; free-fly via WASD while orbiting)
// ---------------------------------------------------------------------------
namespace {

glm::vec3 euler_direction(float yawDeg, float pitchDeg) {
    const float yawRad = glm::radians(yawDeg);
    const float pitchRad = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(
        std::cos(yawRad) * std::cos(pitchRad),
        std::sin(pitchRad),
        std::sin(yawRad) * std::cos(pitchRad)));
}

// Distance from a point to a 2D segment (screen space).
float dist_point_segment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-8f) return glm::length(p - a);
    const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

constexpr glm::vec3 kAxisDirs[3] = { {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} };
constexpr glm::vec3 kAxisColors[3] = { {1.0f, 0.25f, 0.25f}, {0.30f, 1.0f, 0.45f}, {0.35f, 0.62f, 1.0f} };

} // namespace

glm::vec3 EditorCamera::get_front() const {
    return euler_direction(yaw, pitch);
}

glm::vec3 EditorCamera::get_right() const {
    const glm::vec3 front = get_front();
    const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
    return right;
}

glm::vec3 EditorCamera::get_up() const {
    return glm::normalize(glm::cross(get_right(), get_front()));
}

glm::mat4 EditorCamera::get_view_matrix() const {
    return glm::lookAt(position, orbitTarget, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 EditorCamera::get_projection_matrix(float aspectRatio) const {
    return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
}

int EditorApplication::run() {
    try {
        init_window();
        init_vulkan();
        init_imgui();
        init_offscreen_target();
        init_scene_pipeline();
        init_geometry_buffers();
        init_default_scene();

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
    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight, tr("Vulkan Engine 1.5 - Gerenciador de Jogos", "Vulkan Engine 1.5 - Game Launcher"), nullptr, nullptr);
    if (!m_window) throw std::runtime_error("Failed to create GLFW window");
}

void EditorApplication::init_vulkan() {
    // VC_EDITOR_VALIDATION=1 enables the Vulkan validation layers + default
    // debug messenger (messages go to stderr). VC_EDITOR_SKIP_LAUNCHER=1 skips
    // the launcher hub so the 3D viewport is exercised immediately.
    const bool validate = std::getenv("VC_EDITOR_VALIDATION") != nullptr;
    vkb::InstanceBuilder builder;
    builder.set_app_name("Vulkan Engine Studio")
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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.FontGlobalScale = 1.15f; // Typography readability scale

    // Apply Premium Modern Dark Slate & Indigo Palette
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    // High Contrast Dark Obsidian Colors
    style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);            // Pure White Text
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.60f, 0.70f, 1.00f);    // High contrast grey
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);        // Deep Pitch Obsidian #14171F
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.15f, 1.00f);         // Container Background #1A1D26
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.18f, 0.98f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.23f, 0.32f, 1.00f);          // Crisp Border #333B52
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.22f, 1.00f);         // Inputs #242938
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.35f, 1.00f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.32f, 0.45f, 1.00f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.07f, 0.10f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.10f, 0.14f, 1.00f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.18f, 0.21f, 0.30f, 1.00f);         // Selected Items
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.32f, 0.45f, 1.00f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.39f, 0.40f, 0.95f, 1.00f);     // Active Indigo Glow #6366F1
    style.Colors[ImGuiCol_Button] = ImVec4(0.16f, 0.19f, 0.28f, 1.00f);         // Buttons
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.39f, 0.40f, 0.95f, 1.00f);  // Indigo Glow Hover
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.27f, 0.79f, 1.00f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.10f, 0.11f, 0.15f, 1.00f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.32f, 0.45f, 1.00f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.39f, 0.40f, 0.95f, 1.00f);       // Tab Highlight Indigo #6366F1

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
}

void EditorApplication::init_default_scene() {
    m_editorScene = std::make_unique<Scene>("Untitled Scene");

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

    if (std::getenv("VC_EDITOR_SKIP_LAUNCHER") != nullptr) {
        m_inLauncherMode = false;
        glfwSetWindowTitle(m_window, ("Vulkan Engine Studio 1.5 - [" + m_currentProjectName + "]").c_str());
    }

    // VC_EDITOR_TEST_MATERIAL=1: exercise the material-graph viewport path
    // (graph → GLSL → glslc → pipeline → per-entity UBO → draw) headlessly.
    if (std::getenv("VC_EDITOR_TEST_MATERIAL") != nullptr) {
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
        void* data = nullptr;
        vkMapMemory(m_device, cubeRes.vb.memory, 0, vbSize, 0, &data);
        std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
        vkUnmapMemory(m_device, cubeRes.vb.memory);
        vkMapMemory(m_device, cubeRes.ib.memory, 0, ibSize, 0, &data);
        std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
        vkUnmapMemory(m_device, cubeRes.ib.memory);
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

void EditorApplication::setup_play_runtime() {
    teardown_play_runtime();
    Scene* playScene = m_playMode.get_active_scene();
    if (!playScene) return;
    for (const auto& [id, rb] : playScene->rigidbodyComponents) {
        Physics::BodyDesc desc;
        desc.motion = rb.isKinematic ? Physics::MotionType::Kinematic : Physics::MotionType::Dynamic;
        desc.mass = std::max(rb.mass, 0.01f);
        desc.collider.friction = rb.friction;
        desc.collider.restitution = rb.restitution;
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            desc.position = tit->second.position;
            desc.rotation = glm::quat(glm::radians(tit->second.rotation));
        }
        const Physics::BodyHandle handle = m_playPhysics.create_body(desc);
        if (handle != Physics::InvalidBody) m_playBodies[id] = handle;
    }

    // Play particles (Fase 8): one ParticleSimulation emitter per
    // ParticleEmitterComponent entity, positioned at the world transform.
    for (const auto& [id, pe] : playScene->particleEmitterComponents) {
        if (!pe.emitting) continue;
        Engine::Gameplay::ParticleEmitterDesc desc;
        desc.direction = glm::normalize(pe.direction);
        desc.coneAngle = pe.coneAngle;
        desc.rate = pe.rate;
        desc.speedMin = pe.speedMin;
        desc.speedMax = pe.speedMax;
        desc.lifetimeMin = pe.lifetimeMin;
        desc.lifetimeMax = pe.lifetimeMax;
        desc.sizeStart = pe.sizeStart;
        desc.sizeEnd = pe.sizeEnd;
        desc.colorStart = pe.colorStart;
        desc.colorEnd = pe.colorEnd;
        desc.acceleration = pe.acceleration;
        desc.drag = pe.drag;
        desc.turbulence = pe.turbulence;
        desc.restitution = pe.restitution;
        desc.collide = pe.collide;
        desc.emitting = pe.emitting;
        const auto tit = playScene->transformComponents.find(id);
        desc.position = (tit != playScene->transformComponents.end())
                            ? tit->second.position + pe.position
                            : pe.position;
        m_playEmitters[id] = m_playParticles.add_emitter(desc);
        if (pe.burstCount > 0) m_playParticles.emit_burst(m_playEmitters[id], pe.burstCount);
    }

    // Play vehicles (Fase 8): chassis body + four wheels derived from the
    // component's wheelBase/trackWidth, driven by the arrow keys in play.
    for (const auto& [id, veh] : playScene->vehicleComponents) {
        if (!veh.enabled) continue;
        Physics::BodyDesc chassis;
        chassis.motion = Physics::MotionType::Dynamic;
        chassis.mass = std::max(veh.mass, 1.0f);
        chassis.collider.shape = Physics::BoxShape{{veh.wheelBase * 0.35f, 0.35f, veh.trackWidth * 0.35f}};
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            chassis.position = tit->second.position;
            chassis.rotation = glm::quat(glm::radians(tit->second.rotation));
        }
        const Physics::BodyHandle body = m_playPhysics.create_body(chassis);
        if (body == Physics::InvalidBody) continue;
        m_playVehicleChassis[id] = body;
        const float halfBase = veh.wheelBase * 0.5f;
        const float halfTrack = veh.trackWidth * 0.5f;
        const glm::vec3 locals[4] = {
            {-halfBase, -0.1f, -halfTrack}, {-halfBase, -0.1f, halfTrack},
            {halfBase, -0.1f, -halfTrack},  {halfBase, -0.1f, halfTrack},
        };
        std::vector<Engine::Gameplay::WheelDesc> wheels(4);
        for (int i = 0; i < 4; ++i) {
            wheels[i].localPosition = locals[i];
            wheels[i].radius = veh.wheelRadius;
            wheels[i].suspensionRestLength = veh.suspensionRest;
            wheels[i].maxDriveForce = veh.enginePower;
            wheels[i].maxBrakeForce = veh.brakeForce;
            wheels[i].maxSteerAngle = veh.maxSteerAngle;
            wheels[i].steering = i < 2;
            wheels[i].driven = veh.frontWheelDrive ? i < 2 : i >= 2;
        }
        m_playVehicles.emplace(id, Engine::Gameplay::VehicleRuntime(body, std::move(wheels)));
    }

    // Play ragdolls (Fase 6): physics bodies per bone. With fromSkeleton set,
    // the bones come from the entity's skin skeleton (a sibling Skeleton asset
    // matching the mesh stem); otherwise a two-bone fallback is used. The play
    // physics simulates them each frame; the pose drives skinned rendering.
    for (const auto& [id, rg] : playScene->ragdollComponents) {
        if (!rg.enabled) continue;
        glm::vec3 rootPos{0.0f};
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) rootPos = tit->second.position;
        rootPos += rg.spawnOffset;
        std::vector<Physics::RagdollBoneDesc> bones;
        bool fromSkin = false;
        if (rg.fromSkeleton) {
            std::string meshStem;
            if (const auto mit = playScene->meshRendererComponents.find(id); mit != playScene->meshRendererComponents.end()) {
                for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
                    if (asset.type == AssetType::Mesh && asset.id == mit->second.meshAssetID) {
                        meshStem = asset.sourcePath.stem().string();
                        break;
                    }
                }
            }
            if (!meshStem.empty()) {
                for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
                    if (asset.type != AssetType::Skeleton || asset.sourcePath.stem().string() != meshStem) continue;
                    SkeletonAsset skeleton;
                    if (AnimationAssetIO::load_skeleton(skeleton, asset.cookedPath)) {
                        bones = Physics::build_ragdoll_bones(skeleton, rg.massPerBone);
                        fromSkin = !bones.empty();
                    }
                    break;
                }
            }
            if (!fromSkin) {
                std::cout << "[Editor] Ragdoll entity " << id.to_string() << ": no sibling skeleton for mesh '"
                          << meshStem << "' — using two-bone fallback\n";
            }
        }
        if (bones.empty()) {
            bones.push_back(Physics::RagdollBoneDesc{"Root", "", glm::vec3(0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, rg.massPerBone, glm::vec3(0.0f)});
            bones.push_back(Physics::RagdollBoneDesc{"Tip", "Root", glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, rg.massPerBone, glm::vec3(0.0f)});
        }
        Physics::Ragdoll ragdoll;
        if (ragdoll.create(m_playPhysics, bones, rootPos)) {
            m_playRagdolls.emplace(id, std::move(ragdoll));
            std::cout << "[Editor] Play ragdoll active (entity=" << id.to_string() << ", bones="
                      << bones.size() << (fromSkin ? ", from skin skeleton)\n" : ", fallback)\n");
        }
    }

    // Play missions (Fase 8): Start -> SetObjective -> WaitForEvent -> Complete.
    // The completeEvent is dispatched to the mission system by the play tick
    // when another component raises it (e.g. a weapon kill or script emit).
    for (const auto& [id, mc] : playScene->missionComponents) {
        std::vector<Engine::Gameplay::MissionNode> nodes;
        nodes.push_back(Engine::Gameplay::start_node("start", "obj"));
        nodes.push_back(Engine::Gameplay::set_objective_node("obj", "objective", mc.objectiveText, mc.objectiveTarget, "wait"));
        nodes.push_back(Engine::Gameplay::wait_for_event_node("wait", mc.completeEvent, 1, "done"));
        nodes.push_back(Engine::Gameplay::complete_mission_node("done"));
        Engine::Gameplay::Mission mission("mission_" + id.to_string(), mc.missionId, std::move(nodes));
        m_playMissions.register_mission(std::move(mission));
        m_playMissionIds[id] = mc.missionId;
        if (mc.autoStart) m_playMissions.start("mission_" + id.to_string());
    }

    // Play dialogues (Fase 8): a one-node graph with a single choice that can
    // chain to another dialogue; played on start when playOnStart is set.
    for (const auto& [id, dc] : playScene->dialogueComponents) {
        Engine::Gameplay::DialogueGraph graph;
        graph.id = dc.dialogueId;
        Engine::Gameplay::DialogueNode node;
        node.id = "line";
        node.line.character = dc.character;
        node.line.text = dc.line;
        if (!dc.choiceText.empty()) {
            Engine::Gameplay::DialogueChoice choice;
            choice.text = dc.choiceText;
            choice.nextNode = dc.nextDialogueId;   // empty = end
            node.choices.push_back(std::move(choice));
        }
        graph.nodes.push_back(std::move(node));
        m_playDialogues.register_graph(std::move(graph));
        m_playDialogueIds[id] = dc.dialogueId;
        if (dc.playOnStart) m_playDialogues.play(dc.dialogueId);
    }

    // Play audio (Fase 8): resolve the .ogg through the asset registry, decode
    // it into an AudioClip and start a voice on the mixer (spatial vs the
    // camera listener). Voices advance when the mixer is rendered each tick.
    for (const auto& [id, ac] : playScene->audioComponents) {
        if (!ac.playOnStart || ac.clipPath.empty()) continue;
        std::filesystem::path clipSource;
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.type == AssetType::Audio && asset.sourcePath == ac.clipPath) {
                clipSource = asset.sourcePath;
                break;
            }
        }
        if (clipSource.empty()) {
            std::cout << "[Editor] Play audio: no registered asset for '" << ac.clipPath << "'\n";
            continue;
        }
        const auto decoded = Engine::Audio::OggDecoder::decode_file(clipSource);
        if (!decoded || !decoded->valid()) {
            std::cout << "[Editor] Play audio: failed to decode '" << clipSource.string() << "'\n";
            continue;
        }
        auto clip = std::make_shared<Engine::Audio::AudioClip>(clipSource.filename().string());
        Engine::Audio::AudioBuffer buffer;
        buffer.sampleRate = decoded->sampleRate;
        buffer.channels = decoded->channels;
        buffer.samples = decoded->samples;
        clip->hot_swap(std::move(buffer));
        Engine::Audio::VoiceDescription desc;
        desc.clip = std::move(clip);
        desc.bus = m_playAudio.master_bus();
        desc.gain = ac.volume;
        desc.pitch = ac.pitch;
        desc.looping = ac.looping;
        desc.spatial = ac.spatial;
        const auto tit = playScene->transformComponents.find(id);
        desc.position = (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        const Engine::Audio::VoiceId voice = m_playAudio.play(std::move(desc));
        m_playVoices[id] = voice;
        std::cout << "[Editor] Play audio voice started ('" << clipSource.filename().string() << "')\n";
    }

    // Play destructibles (Fase 8): chunkCount boxes laid out in a square grid
    // around the entity transform; weapon hits apply radial damage.
    for (const auto& [id, dc] : playScene->destructionComponents) {
        if (!dc.enabled) continue;
        const glm::vec3 center = [&]() {
            const auto tit = playScene->transformComponents.find(id);
            return (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        }();
        const glm::quat rotation = [&]() {
            const auto tit = playScene->transformComponents.find(id);
            return (tit != playScene->transformComponents.end())
                       ? glm::quat(glm::radians(tit->second.rotation))
                       : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }();
        const uint32_t n = std::max(dc.chunkCount, 1u);
        const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(n))));
        std::vector<Engine::Gameplay::DestructionChunkDesc> chunks;
        chunks.reserve(n);
        const glm::vec3 half = dc.chunkSize * 0.5f;
        for (uint32_t i = 0; i < n; ++i) {
            const int cx = static_cast<int>(i % cols);
            const int cy = static_cast<int>(i / cols);
            const glm::vec3 local = glm::vec3((static_cast<float>(cx) - (cols - 1) * 0.5f) * dc.chunkSize.x,
                                              (static_cast<float>(cy) - (cols - 1) * 0.5f) * dc.chunkSize.y, 0.0f);
            Engine::Gameplay::DestructionChunkDesc chunk;
            chunk.localPosition = local;
            chunk.halfExtents = half;
            chunk.mass = 1.0f;
            chunk.health = dc.chunkHealth;
            chunks.push_back(chunk);
        }
        Engine::Gameplay::DestructibleRuntime runtime;
        if (runtime.create(m_playPhysics, center, rotation, chunks)) {
            m_playDestructibles.emplace(id, std::move(runtime));
        }
    }

    // Play navigation (Fase 8): bake a NavigationGrid tile (blockers from play
    // physics bodies) and drive a NavigationAgent toward the primary camera
    // entity each frame; the agent position writes back to the transform.
    for (const auto& [id, nc] : playScene->navigationComponents) {
        if (!nc.enabled) continue;
        const auto tit = playScene->transformComponents.find(id);
        const glm::vec3 start = (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        Engine::NavigationGrid grid(nc.gridWidth, nc.gridHeight, nc.cellSize);
        for (const auto& [bid, handle] : m_playBodies) {
            (void)bid;
            Physics::RigidBody* body = m_playPhysics.body(handle);
            if (!body) continue;
            const glm::vec3 half = std::visit([](const auto& s) -> glm::vec3 {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, Physics::BoxShape>) return s.halfExtents;
                else if constexpr (std::is_same_v<T, Physics::SphereShape>) return glm::vec3(s.radius);
                else return glm::vec3(s.radius, s.halfHeight, s.radius);
            }, body->collider.shape);
            const glm::vec3 min = body->position - half;
            const glm::vec3 max = body->position + half;
            for (float wx = min.x; wx <= max.x; wx += nc.cellSize) {
                for (float wz = min.z; wz <= max.z; wz += nc.cellSize) {
                    const auto cell = grid.cell_at({wx, 0.0f, wz});
                    if (cell) grid.set_blocked(*cell, true);
                }
            }
        }
        m_playNavWorld.load_tile({0, 0}, std::move(grid));
        Engine::NavigationAgent agent;
        agent.position = start;
        agent.speed = nc.agentSpeed;
        m_playNavAgents.emplace(id, agent);
    }

    // Play-mode script: watch the scene's companion .script and compile it into
    // the play VM. OnStart starts immediately; a "Tick" event runs each frame
    // (same convention as the packaged game's player controller).
    m_playScriptPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Content" / "Scenes" / "Initial.script";
    if (m_playScriptReloader.watch(m_playScriptPath)) {
        ScriptGraphAsset graph;
        if (graph.load(m_playScriptPath)) {
            const auto compiled = ScriptCompiler::compile(graph);
            if (compiled) {
                m_playScript.load(std::move(compiled.program));
                m_playScript.start_event("OnStart");
                m_playScriptLoaded = true;
                m_scriptDebugGraph = graph;
                m_scriptDebugger.attach(m_playScript);
                m_scriptDebuggerAttached = true;
                m_scriptPauseRequested = false;
                std::cout << "[Editor] Play script loaded: " << m_playScriptPath.string() << std::endl;
            }
        }
    }
}

void EditorApplication::tick_play_runtime(float deltaTime) {
    const PlayState state = m_playMode.get_state();
    if (state != PlayState::Play && state != PlayState::Simulate) return;
    Scene* playScene = m_playMode.get_active_scene();
    if (!playScene) return;
    m_playPhysics.step(deltaTime);
    for (const auto& [id, handle] : m_playBodies) {
        Physics::RigidBody* body = m_playPhysics.body(handle);
        if (!body) continue;
        auto tit = playScene->transformComponents.find(id);
        if (tit == playScene->transformComponents.end()) continue;
        tit->second.position = body->position;
        tit->second.rotation = glm::degrees(glm::eulerAngles(body->rotation));
    }

    // Hot reload: recompile + swap the program when the .script changes on
    // disk (variables survive — load() keeps the variable map).
    if (m_playScriptLoaded) {
        std::string reloadError;
        if (m_playScriptReloader.reload_if_changed(m_playScript, &reloadError)) {
            std::cout << "[Editor] Script hot-reloaded: " << m_playScriptPath.filename().string()
                      << (reloadError.empty() ? "" : " (" + reloadError + ")") << std::endl;
            m_playScript.start_event("OnStart");
        }
        // Debugger-aware tick: hold on a panel pause or a breakpoint pause;
        // otherwise drive the VM through the debugger so breakpoints halt it
        // and the panel stays in sync (variables/ip/call stack).
        const bool breakpointPaused = m_playScript.status() == VMStatus::Paused;
        if (!m_scriptPauseRequested && !breakpointPaused) {
            if (m_scriptDebuggerAttached) m_scriptDebugger.continue_run(10000, deltaTime);
            else m_playScript.run(deltaTime, 10000);
            if (m_playScript.status() == VMStatus::Paused) return; // hit a breakpoint
            std::vector<std::string> emitted;
            m_playScript.consume_emitted_events(emitted);
            for (const std::string& event : emitted) m_playScript.start_event(event);
        if (m_playScript.has_event("Tick")) {
            m_playScript.start_event("Tick");
            if (m_scriptDebuggerAttached) m_scriptDebugger.continue_run(10000, deltaTime);
            else m_playScript.run(deltaTime, 10000);
        }
    }

    // Play weapons (Fase 8): one WeaponRuntime per WeaponComponent entity,
    // fired with the viewport camera ray against the play physics on SPACE.
    const bool fireHeld = glfwGetKey(m_window, GLFW_KEY_SPACE) == GLFW_PRESS;
    const glm::mat4 camView = m_editorCamera.get_view_matrix();
    const glm::vec3 camFront = glm::normalize(
        glm::vec3(-camView[2][0], -camView[2][1], -camView[2][2]));
    for (const auto& [id, comp] : playScene->weaponComponents) {
        auto it = m_playWeapons.find(id);
        if (it == m_playWeapons.end()) {
            Engine::WeaponDefinition def;
            def.id = id;
            def.name = "Scene Weapon";
            def.fireMode = comp.automatic ? Engine::FireMode::Automatic : Engine::FireMode::Single;
            def.magazineSize = comp.magazineSize;
            def.reserveAmmo = comp.reserveAmmo;
            def.roundsPerMinute = comp.roundsPerMinute;
            def.damage = comp.damage;
            def.range = 120.0f;
            def.spreadDegrees = comp.spreadDegrees;
            def.hitscan = comp.hitscan;
            it = m_playWeapons.emplace(id, Engine::WeaponRuntime(std::move(def))).first;
            it->second.set_raycast([this](const glm::vec3& o, const glm::vec3& d, float maxDist)
                                       -> std::optional<Engine::WeaponHit> {
                const auto hit = m_playPhysics.raycast(o, d, maxDist);
                if (!hit) return std::nullopt;
                Engine::WeaponHit out;
                out.position = hit->point;
                out.normal = hit->normal;
                out.distance = hit->distance;
                return out;
            });
        }
        if (fireHeld) it->second.trigger_pressed(m_editorCamera.position, camFront);
        else it->second.trigger_released();
        it->second.update(deltaTime, m_editorCamera.position, camFront);
    }
    if (fireHeld && !m_playWeaponStatusLogged && !m_playWeapons.empty()) {
        m_playWeaponStatusLogged = true;
        std::cout << "[Editor] Play weapon firing via physics raycast ("
                  << m_playWeapons.size() << " weapon entity/entities)\n";
    }

    // Play particles (Fase 8): keep each emitter at its entity's world
    // position and step the simulation against the play physics.
    for (const auto& [id, emitter] : m_playEmitters) {
        auto* desc = m_playParticles.emitter(emitter);
        if (!desc) continue;
        const auto tit = playScene->transformComponents.find(id);
        const auto pe = playScene->particleEmitterComponents.find(id);
        const glm::vec3 localPos =
            (pe != playScene->particleEmitterComponents.end()) ? pe->second.position : glm::vec3(0.0f);
        desc->position = (tit != playScene->transformComponents.end())
                             ? tit->second.position + localPos
                             : localPos;
    }
    m_playParticles.update(deltaTime, &m_playPhysics);

    // Play vehicles (Fase 8): drive with the arrow keys.
    const bool throttle = glfwGetKey(m_window, GLFW_KEY_UP) == GLFW_PRESS;
    const bool brake = glfwGetKey(m_window, GLFW_KEY_DOWN) == GLFW_PRESS;
    const float steer = (glfwGetKey(m_window, GLFW_KEY_RIGHT) == GLFW_PRESS ? 1.0f : 0.0f) -
                        (glfwGetKey(m_window, GLFW_KEY_LEFT) == GLFW_PRESS ? 1.0f : 0.0f);
    for (auto& [id, vehicle] : m_playVehicles) {
        (void)id;
        Engine::Gameplay::VehicleInput input;
        input.throttle = throttle ? 1.0f : 0.0f;
        input.brake = brake ? 1.0f : 0.0f;
        input.steering = steer;
        vehicle.set_input(input);
        vehicle.update(m_playPhysics, deltaTime);
    }

    // Play missions (Fase 8): step the graph and mirror the live state back
    // to the component. Events emitted by the play script (consume_emitted)
    // are dispatched to the mission system, so authored script events can
    // complete missions.
    m_playMissions.update(deltaTime);
    for (auto& [id, mc] : playScene->missionComponents) {
        const Engine::Gameplay::Mission* mission = m_playMissions.mission("mission_" + id.to_string());
        mc.active = mission && mission->is_active();
    }
    {
        std::vector<std::string> emitted;
        if (m_playScriptLoaded) m_playScript.consume_emitted_events(emitted);
        for (const std::string& event : emitted) m_playMissions.dispatch_event(event);
    }

    // Play dialogues (Fase 8): mirror the playing state.
    for (auto& [id, dc] : playScene->dialogueComponents) {
        dc.playing = m_playDialogues.is_playing();
    }

    // Play audio (Fase 8): keep the listener on the camera and render one
    // mix block per frame so voices advance; drop voices that finished.
    m_playAudio.set_listener(m_editorCamera.position, camFront);
    for (const auto& [id, voice] : m_playVoices) {
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            m_playAudio.set_voice_position(voice, tit->second.position);
        }
        auto ac = playScene->audioComponents.find(id);
        if (ac != playScene->audioComponents.end()) ac->second.playing = m_playAudio.is_active(voice);
    }
    m_playAudio.render(1024);

    // Play destructibles (Fase 8): weapon hits from this frame apply radial
    // damage (chunks detach with an impulse) and the destroyed flag syncs.
    std::vector<glm::vec3> hitPoints;
    for (const auto& [id, comp] : playScene->weaponComponents) {
        (void)id;
        auto it = m_playWeapons.find(id);
        if (it == m_playWeapons.end()) continue;
        for (const Engine::WeaponHit& hit : it->second.hits()) hitPoints.push_back(hit.position);
        it->second.clear_hits();
    }
    for (auto& [id, runtime] : m_playDestructibles) {
        auto dc = playScene->destructionComponents.find(id);
        for (const glm::vec3& point : hitPoints) {
            runtime.apply_radial_damage(m_playPhysics, point, dc->second.damageRadius, 25.0f, dc->second.damageImpulse);
        }
        if (dc != playScene->destructionComponents.end()) {
            dc->second.destroyed = runtime.fully_destroyed();
        }
    }

    // Play navigation (Fase 8): repath toward the camera entity when the agent
    // arrives (or the target moved), then write the agent position back.
    glm::vec3 target{0.0f};
    bool haveTarget = false;
    for (const auto& [cid, cam] : playScene->cameraComponents) {
        (void)cam;
        const auto tit = playScene->transformComponents.find(cid);
        if (tit != playScene->transformComponents.end()) {
            target = tit->second.position;
            haveTarget = true;
            break;
        }
    }
    if (!haveTarget) target = m_editorCamera.position;
    for (auto& [id, agent] : m_playNavAgents) {
        if (agent.reached_destination() || glm::distance(agent.position, target) > 1.0f) {
            Engine::NavigationPath path = m_playNavWorld.find_path(agent.position, target);
            if (path.success) agent.set_path(std::move(path));
        }
        agent.update(deltaTime);
        auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) tit->second.position = agent.position;
    }
}
}

void EditorApplication::teardown_play_runtime() {
    for (const auto& [id, handle] : m_playBodies) {
        (void)id;
        m_playPhysics.destroy_body(handle);
    }
    m_playBodies.clear();        m_playScriptLoaded = false;
    m_scriptDebugger.detach();
    m_scriptDebuggerAttached = false;
    m_scriptPauseRequested = false;
    m_playWeapons.clear();
    m_playWeaponStatusLogged = false;
    for (const auto& [id, handle] : m_playVehicleChassis) {
        (void)id;
        m_playPhysics.destroy_body(handle);
    }
    m_playVehicleChassis.clear();
    m_playVehicles.clear();
    m_playParticles.clear();
    m_playEmitters.clear();
    for (auto& [id, ragdoll] : m_playRagdolls) {
        (void)id;
        ragdoll.destroy(m_playPhysics);
    }
    m_playRagdolls.clear();
    m_playMissions.clear();
    m_playMissionIds.clear();
    m_playDialogues.clear();
    m_playDialogueIds.clear();
    for (const auto& [id, voice] : m_playVoices) {
        (void)id;
        m_playAudio.stop(voice);
    }
    m_playVoices.clear();
    for (auto& [id, runtime] : m_playDestructibles) {
        (void)id;
        runtime.destroy(m_playPhysics);
    }
    m_playDestructibles.clear();
    m_playNavAgents.clear();
    m_playNavWorld.unload_tile({0, 0});
}

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

        double currentTime = glfwGetTime();
        float deltaTime = static_cast<float>(currentTime - lastTime);
        lastTime = currentTime;
        m_fps = 1.0f / std::max(deltaTime, 0.001f);
        m_frameTimeMs = deltaTime * 1000.0f;

        if (!m_inLauncherMode) {
            update_editor_camera(deltaTime);
            process_viewport_input();
            tick_play_runtime(deltaTime);
        }

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
    vkDeviceWaitIdle(m_device);
}

void EditorApplication::render_frame() {
    vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);

    // A pick requested from the previous frame is resolved before this frame's
    // scene pass so the freshly selected entity is highlighted immediately.
    if (m_pickRequested && !m_inLauncherMode) {
        perform_pick_readback();
        m_pickRequested = false;
    }

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
        }
        return;
    }

    vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

    VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &beginInfo);

    if (!m_inLauncherMode) {
        recreate_offscreen_if_needed(
            static_cast<uint32_t>(std::max(1.0f, m_viewportImageSize.x)),
            static_cast<uint32_t>(std::max(1.0f, m_viewportImageSize.y)));
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
        m_editorGui.update(0.0f);
        draw_dockspace();
        draw_menu_bar();
        draw_toolbar();
        draw_hierarchy_panel();
        draw_inspector_panel();
        draw_viewport_panel();
        draw_content_browser_panel();
#if VC_ENABLE_VOXEL_PLUGIN
        draw_voxel_tool_panel();
#endif
        draw_console_panel();
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
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
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

void EditorApplication::draw_project_launcher() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("Project Launcher Hub", nullptr, flags);

    // Modern Header Banner
    ImGui::SetCursorPosY(35.0f);
    ImGui::SetCursorPosX((viewport->WorkSize.x - 550.0f) * 0.5f);
    ImGui::TextColored(ImVec4(0.39f, 0.40f, 0.95f, 1.00f), "%s", tr("GERENCIADOR DE JOGOS VULKAN ENGINE", "VULKAN ENGINE GAME LAUNCHER"));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.4f, 1.0f), "[v1.5.0]");

    ImGui::SetCursorPosX((viewport->WorkSize.x - 550.0f) * 0.5f);
    ImGui::TextDisabled("%s", tr("Escolha um jogo para editar ou crie um novo projeto", "Select a game to edit or create a new project"));
    ImGui::Separator();
    ImGui::Spacing();

    // Centered Projects Card
    ImGui::SetCursorPosX((viewport->WorkSize.x - 720.0f) * 0.5f);
    ImGui::BeginChild("ProjectsListContainer", ImVec2(720, 480), true, ImGuiWindowFlags_None);

    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.95f, 1.0f), "%s", tr("Seus Jogos e Projetos:", "Your Games & Projects:"));
    ImGui::Separator();
    ImGui::Spacing();

    struct ProjectItem {
        std::string name;
        std::string path;
        std::string preset;
        std::string badge;
        ImVec4 badgeColor;
        std::string lastModified;
    };

    static const std::vector<ProjectItem> projects = {
        { "EmptyProject", "Projects/EmptyProject", tr("Projeto vazio reutilizável", "Reusable empty project"), tr("[GENÉRICO]", "[GENERIC]"), ImVec4(0.4f, 0.7f, 1.0f, 1.0f), tr("Novo", "New") }
    };

    for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
        const auto& proj = projects[i];
        bool isSelected = (m_selectedProjectIndex == i);

        ImGui::PushID(i);
        if (ImGui::Selectable("##ProjectSelectable", isSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 80))) {
            m_selectedProjectIndex = i;
            m_currentProjectName = proj.name;
            if (ImGui::IsMouseDoubleClicked(0)) {
                m_inLauncherMode = false; // Launch Engine Studio
                glfwSetWindowTitle(m_window, ("Vulkan Engine Studio 1.5 - [" + m_currentProjectName + "]").c_str());
            }
        }
        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::TextColored(isSelected ? ImVec4(0.4f, 0.7f, 1.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "[JOGO]  %s", proj.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(proj.badgeColor, "%s", proj.badge.c_str());

        ImGui::TextDisabled("Pasta: %s", proj.path.c_str());
        ImGui::Text("Tipo: %s  |  Modificado: %s", proj.preset.c_str(), proj.lastModified.c_str());
        ImGui::EndGroup();

        ImGui::PopID();
        ImGui::Separator();
    }

    ImGui::EndChild();

    // Launcher Action Buttons
    ImGui::SetCursorPosY(viewport->WorkSize.y - 75.0f);
    ImGui::SetCursorPosX((viewport->WorkSize.x - 720.0f) * 0.5f);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.39f, 0.40f, 0.95f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.49f, 0.50f, 1.00f, 1.00f));
    if (ImGui::Button(tr("ABRIR NO EDITOR", "LAUNCH ENGINE STUDIO"), ImVec2(250, 44))) {
        m_inLauncherMode = false;
        glfwSetWindowTitle(m_window, ("Vulkan Engine Studio 1.5 - [" + m_currentProjectName + "]").c_str());
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    if (ImGui::Button(tr("+ Criar Novo Jogo", "+ Create New Game"), ImVec2(220, 44))) {
        m_inLauncherMode = false;
        glfwSetWindowTitle(m_window, "Vulkan Engine Studio 1.5 - [Novo Jogo]");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Procurar Pasta...", "Browse Folder..."), ImVec2(220, 44))) {}

    ImGui::End();
}

void EditorApplication::draw_dockspace() {
    static bool firstTime = true;
    ImGuiID dockspace_id = ImGui::GetID("VulkanEngineStudioDockspace");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float toolbarHeight = 36.0f;
    ImVec2 dockPos = ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + toolbarHeight);
    ImVec2 dockSize = ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - toolbarHeight);

    ImGui::SetNextWindowPos(dockPos);
    ImGui::SetNextWindowSize(dockSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("Vulkan Engine Studio Shell", nullptr, host_window_flags);
    ImGui::PopStyleVar(3);

    ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    if (firstTime) {
        firstTime = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, dockspace_flags | ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, dockSize);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.22f, nullptr, &dock_main_id);
        ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.25f, nullptr, &dock_main_id);
        ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down, 0.32f, nullptr, &dock_main_id);

        ImGui::DockBuilderDockWindow(tr("Objetos do Jogo", "World Hierarchy"), dock_left);
        ImGui::DockBuilderDockWindow(tr("Propriedades do Objeto", "Inspector"), dock_right);
        ImGui::DockBuilderDockWindow(tr("Visualização 3D", "Scene Viewport"), dock_main_id);
        ImGui::DockBuilderDockWindow(tr("Arquivos do Projeto", "Project Content Browser"), dock_bottom);
        ImGui::DockBuilderDockWindow(tr("Mensagens do Sistema", "Console & Profiler"), dock_bottom);
#if VC_ENABLE_VOXEL_PLUGIN
        ImGui::DockBuilderDockWindow(tr("Escultura de Blocos", "Voxel Sculpting Tools"), dock_left);
#endif

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::End();
}

void EditorApplication::draw_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(tr("Arquivo", "File"))) {
            if (ImGui::MenuItem(tr("Gerenciador de Jogos", "Game Launcher Hub"))) {
                m_inLauncherMode = true;
                glfwSetWindowTitle(m_window, tr("Vulkan Engine 1.5 - Gerenciador de Jogos", "Vulkan Engine 1.5 - Game Launcher"));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Novo Jogo", "New Scene"), "Ctrl+N")) {
                m_editorScene = std::make_unique<Scene>("Untitled Scene");
                init_default_scene();
            }
            if (ImGui::MenuItem(tr("Abrir Jogo...", "Open Scene..."), "Ctrl+O")) {}
            if (ImGui::MenuItem(tr("Salvar Jogo", "Save Scene"), "Ctrl+S")) {
                if (m_editorScene) m_editorScene->save_to_file("assets/scenes/active_world.scene");
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Exportar Jogo Pronto (.exe)", "Export Executable Game Build..."))) {
                run_game_build();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Sair", "Exit Studio"), "Alt+F4")) {
                glfwSetWindowShouldClose(m_window, true);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Editar", "Edit"))) {
            if (ImGui::MenuItem(tr("Desfazer", "Undo"), "Ctrl+Z", false, m_undo.can_undo())) {
                m_undo.undo();
            }
            if (ImGui::MenuItem(tr("Refazer", "Redo"), "Ctrl+Y", false, m_undo.can_redo())) {
                m_undo.redo();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(tr("Configurações", "Settings"))) {
                if (ImGui::BeginMenu(tr("Idioma / Language", "Language"))) {
                    bool isPt = (m_currentLanguage == EngineLanguage::PT_BR);
                    bool isEn = (m_currentLanguage == EngineLanguage::EN_US);
                    if (ImGui::MenuItem("Português (Brasil)", nullptr, isPt)) {
                        m_currentLanguage = EngineLanguage::PT_BR;
                    }
                    if (ImGui::MenuItem("English (US)", nullptr, isEn)) {
                        m_currentLanguage = EngineLanguage::EN_US;
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Adicionar Objeto", "GameObject"))) {
            if (ImGui::MenuItem(tr("Objeto Vazio", "Create Empty Entity"))) {
                if (m_editorScene) {
                    Entity ent = m_editorScene->create_entity(tr("Novo Objeto", "New Entity"));
                    m_selectedEntity = ent;
                }
            }
            if (ImGui::MenuItem(tr("Objeto 3D > Cubo", "3D Object > Cube"))) {
                if (m_editorScene) {
                    Entity cube = m_editorScene->create_entity(tr("Cubo 3D", "Cube"));
                    m_editorScene->meshRendererComponents[cube.get_id()] = MeshRendererComponent{};
                    m_selectedEntity = cube;
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz do Sol", "Light > Directional Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz do Sol", "Directional Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{};
                    m_selectedEntity = light;
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz de Lâmpada", "Light > Point Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz de Lâmpada", "Point Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(1.0f, 0.8f, 0.4f), 5000.0f, 15.0f, true };
                    m_selectedEntity = light;
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz Spot", "Light > Spot Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz Spot", "Spot Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot };
                    m_selectedEntity = light;
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz de Área", "Light > Area Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz de Área", "Area Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area };
                    m_selectedEntity = light;
                }
            }
#if VC_ENABLE_VOXEL_PLUGIN
            if (ImGui::MenuItem(tr("Blocos > Mundo de Blocos", "Voxel > Voxel Terrain Volume"))) {
                if (m_editorScene) {
                    Entity voxel = m_editorScene->create_entity(tr("Mundo de Blocos", "Voxel Volume"));
                    m_editorScene->voxelVolumeComponents[voxel.get_id()] = VoxelVolumeComponent{};
                    m_selectedEntity = voxel;
                }
            }
#endif
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Janelas", "Window"))) {
            ImGui::MenuItem(tr("Visualização 3D", "Scene Viewport"), nullptr, nullptr);
            ImGui::MenuItem(tr("Objetos do Jogo", "World Hierarchy"), nullptr, nullptr);
            ImGui::MenuItem(tr("Propriedades do Objeto", "Inspector"), nullptr, nullptr);
            ImGui::MenuItem(tr("Arquivos do Projeto", "Project Content Browser"), nullptr, nullptr);
#if VC_ENABLE_VOXEL_PLUGIN
            ImGui::MenuItem(tr("Escultura de Blocos", "Voxel Sculpting Tools"), nullptr, nullptr);
#endif
            ImGui::MenuItem(tr("Mensagens do Sistema", "Console & Profiler"), nullptr, nullptr);
            ImGui::MenuItem(tr("Debugger de Scripts", "Script Debugger"), nullptr, &m_showScriptDebugger);
            ImGui::MenuItem(tr("Canvas de Scripts", "Script Canvas"), nullptr, &m_showScriptCanvas);
            ImGui::Separator();
            ImGui::MenuItem(tr("Editores Especializados", "Specialized Editors"), nullptr, &m_specializedEditors.open);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Ajuda", "Help"))) {
            if (ImGui::MenuItem(tr("Manual da Engine", "Vulkan Engine Documentation"))) {}
            if (ImGui::MenuItem(tr("Sobre a Engine", "About Vulkan Engine Studio 1.5"))) {}
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorApplication::draw_toolbar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + ImGui::GetFrameHeight()));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, 36.0f));

    ImGuiWindowFlags toolbarFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.14f, 1.00f));

    ImGui::Begin("##TopSimulationToolbarPanel", nullptr, toolbarFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    float btnWidth = 145.0f;
    float btnHeight = 28.0f;
    float totalWidth = (btnWidth * 3) + (ImGui::GetStyle().ItemSpacing.x * 2.0f);

    ImGui::SetCursorPosX((viewport->WorkSize.x - totalWidth) * 0.5f);
    ImGui::SetCursorPosY(4.0f);

    PlayState state = m_playMode.get_state();

    if (state == PlayState::Edit) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.72f, 0.50f, 1.00f)); // Emerald Green #10B981
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.82f, 0.60f, 1.00f));
        if (ImGui::Button(tr("[ > TESTAR JOGO ]", "[ > PLAY ]"), ImVec2(btnWidth, btnHeight))) {
            m_playMode.start_play(m_editorScene.get());
            setup_play_runtime();
#ifdef _WIN32
            launch_external_game();
#endif
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.93f, 0.26f, 0.26f, 1.00f)); // Crimson Red #EF4444
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.98f, 0.40f, 0.40f, 1.00f));
        if (ImGui::Button(tr("[ [] PARAR ]", "[ [] STOP ]"), ImVec2(btnWidth, btnHeight))) {
            teardown_play_runtime();
            m_playMode.stop_play();
#ifdef _WIN32
            stop_external_game();
#endif
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.96f, 0.62f, 0.04f, 1.00f)); // Amber Gold #F59E0B
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.98f, 0.72f, 0.20f, 1.00f));
    if (ImGui::Button(tr("[ || PAUSAR ]", "[ || PAUSE ]"), ImVec2(btnWidth, btnHeight))) {
        m_playMode.pause_play();
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    if (ImGui::Button(tr("[ >> PASSO ]", "[ >> STEP ]"), ImVec2(btnWidth, btnHeight))) {}

    ImGui::End();
}

void EditorApplication::draw_hierarchy_panel() {
    ImGui::Begin(tr("Objetos do Jogo", "World Hierarchy"));

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.39f, 0.40f, 0.95f, 1.00f));
    if (ImGui::Button(tr("+ Adicionar Objeto", "+ Add Entity"), ImVec2(160, 28))) {
        if (m_editorScene) {
            Entity ent = m_editorScene->create_entity(tr("Novo Objeto", "New Entity"));
            m_selectedEntity = ent;
        }
    }
    ImGui::PopStyleColor();
    ImGui::Separator();

    Scene* scene = m_playMode.get_active_scene();
    if (scene) {
        for (const auto& [id, entity] : scene->get_entities()) {
            ImGuiTreeNodeFlags flags = ((m_selectedEntity == entity) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

            std::string icon = "[Objeto]";
            ImVec4 iconColor = ImVec4(0.7f, 0.7f, 0.8f, 1.0f);
            if (scene->cameraComponents.contains(id)) { icon = "[Visão]"; iconColor = ImVec4(0.37f, 0.64f, 0.98f, 1.0f); }
            else if (scene->lightComponents.contains(id)) { icon = "[Luz]"; iconColor = ImVec4(0.98f, 0.75f, 0.14f, 1.0f); }
            else if (scene->voxelVolumeComponents.contains(id)) { icon = "[Blocos]"; iconColor = ImVec4(0.20f, 0.82f, 0.60f, 1.0f); }

            ImGui::TextColored(iconColor, "%s", icon.c_str());
            ImGui::SameLine();

            bool opened = ImGui::TreeNodeEx(reinterpret_cast<void*>(id.get_high() ^ id.get_low()), flags, "%s", entity.get_name().c_str());

            if (ImGui::IsItemClicked()) {
                m_selectedEntity = entity;
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(tr("Deletar Objeto", "Delete Entity"))) {
                    scene->destroy_entity(id);
                    if (m_selectedEntity == entity) m_selectedEntity = Entity();
                }
                ImGui::EndPopup();
            }

            if (opened) ImGui::TreePop();
        }
    }

    ImGui::End();
}

void EditorApplication::draw_inspector_panel() {
    ImGui::Begin(tr("Propriedades do Objeto", "Inspector"));

    if (!m_selectedEntity.is_valid()) {
        ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected"));
        ImGui::End();
        return;
    }

    Scene* scene = m_playMode.get_active_scene();
    if (!scene) {
        ImGui::End();
        return;
    }

    UUID id = m_selectedEntity.get_id();

    // Entity Header
    char nameBuf[256];
    strncpy(nameBuf, m_selectedEntity.get_name().c_str(), sizeof(nameBuf));
    if (ImGui::InputText(tr("Nome do Objeto", "Object Name"), nameBuf, sizeof(nameBuf))) {
        m_selectedEntity.set_name(nameBuf);
    }
    ImGui::TextDisabled("Código Único: %s", id.to_string().c_str());
    ImGui::Separator();

    // Transform Component
    if (scene->transformComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.22f, 0.74f, 0.97f, 1.0f), "%s", tr("Posição, Rotação e Tamanho", "Transform Component"));
        ImGui::Separator();
        auto& t = scene->transformComponents[id];

        ImGui::TextColored(ImVec4(0.93f, 0.26f, 0.26f, 1.0f), "X"); ImGui::SameLine();
        ImGui::DragFloat("##PosX", &t.position.x, 0.1f); ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "Y"); ImGui::SameLine();
        ImGui::DragFloat("##PosY", &t.position.y, 0.1f); ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.23f, 0.55f, 0.98f, 1.0f), "Z"); ImGui::SameLine();
        ImGui::DragFloat("##PosZ", &t.position.z, 0.1f);

        ImGui::DragFloat3(tr("Rotação (Ângulo)", "Rotation"), &t.rotation.x, 1.0f);
        ImGui::DragFloat3(tr("Tamanho (Escala)", "Scale"), &t.scale.x, 0.1f, 0.01f, 100.0f);
        ImGui::Spacing();
    }

    // Light Component
    if (scene->lightComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.14f, 1.0f), "%s", tr("Iluminação e Luz", "Light Component"));
        ImGui::Separator();
        auto& l = scene->lightComponents[id];
        ImGui::ColorEdit3(tr("Cor da Luz", "Light Color"), &l.color.r);
        ImGui::DragFloat(tr("Brilho (Intensidade)", "Intensity"), &l.intensity, 100.0f, 0.0f, 100000.0f);
        ImGui::DragFloat(tr("Alcance da Luz", "Range"), &l.range, 0.5f, 0.1f, 1000.0f);
        ImGui::Checkbox(tr("Projetar Sombras", "Cast Shadows"), &l.castShadows);
        ImGui::Spacing();
    }

    // Camera Component
    if (scene->cameraComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.65f, 0.55f, 0.98f, 1.0f), "%s", tr("Câmera de Visão", "Camera Component"));
        ImGui::Separator();
        auto& c = scene->cameraComponents[id];
        ImGui::SliderFloat(tr("Campo de Visão (FOV)", "Field of View (FOV)"), &c.fov, 10.0f, 160.0f);
        ImGui::DragFloat(tr("Visão Próxima", "Near Plane"), &c.nearPlane, 0.01f, 0.001f, 10.0f);
        ImGui::DragFloat(tr("Visão Distante", "Far Plane"), &c.farPlane, 10.0f, 10.0f, 10000.0f);
        ImGui::Checkbox(tr("Câmera Principal do Jogo", "Primary Camera"), &c.isPrimary);
        ImGui::Spacing();
    }

    // Rigidbody Component
    if (scene->rigidbodyComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.98f, 0.57f, 0.24f, 1.0f), "%s", tr("Física e Gravidade", "Rigidbody Component"));
        ImGui::Separator();
        auto& r = scene->rigidbodyComponents[id];
        ImGui::DragFloat(tr("Peso (kg)", "Mass (kg)"), &r.mass, 0.5f, 0.01f, 10000.0f);
        ImGui::SliderFloat(tr("Deslize (Fricção)", "Friction"), &r.friction, 0.0f, 1.0f);
        ImGui::SliderFloat(tr("Quique (Elasticidade)", "Restitution"), &r.restitution, 0.0f, 1.0f);
        ImGui::Checkbox(tr("Física Fixa (Sem Mover)", "Is Kinematic"), &r.isKinematic);
        ImGui::Checkbox(tr("Ativar Gravidade", "Use Gravity"), &r.useGravity);
        ImGui::Spacing();
    }

    // Weapon Component (authored in the Weapon panel; the play world fires it).
    if (scene->weaponComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.93f, 0.35f, 0.55f, 1.0f), "%s", tr("Arma (Hitscan)", "Weapon Component"));
        ImGui::Separator();
        auto& w = scene->weaponComponents[id];
        ImGui::DragFloat(tr("Dano", "Damage"), &w.damage, 0.5f, 0.0f, 10000.0f);
        ImGui::DragFloat(tr("Tiros/min", "Rounds Per Minute"), &w.roundsPerMinute, 5.0f, 1.0f, 5000.0f);
        ImGui::DragInt(tr("Pente", "Magazine Size"), reinterpret_cast<int*>(&w.magazineSize), 1, 1, 1000);
        ImGui::DragInt(tr("Reserva", "Reserve Ammo"), reinterpret_cast<int*>(&w.reserveAmmo), 1, 0, 10000);
        ImGui::Checkbox(tr("Automática", "Automatic"), &w.automatic);
        ImGui::SliderFloat(tr("Espalhamento (graus)", "Spread (degrees)"), &w.spreadDegrees, 0.0f, 20.0f);
        ImGui::Checkbox(tr("Hitscan", "Hitscan"), &w.hitscan);
        if (ImGui::Button(tr("Remover Arma", "Remove Weapon"))) scene->weaponComponents.erase(id);
        ImGui::Spacing();
    }

    // Particle Emitter Component (authored in the Particle panel; the play
    // world instantiates a ParticleSimulation emitter at the entity origin).
    if (scene->particleEmitterComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.55f, 1.0f), "%s", tr("Emissor de Partículas", "Particle Emitter Component"));
        ImGui::Separator();
        auto& p = scene->particleEmitterComponents[id];
        ImGui::DragFloat3(tr("Posição (local)", "Position (local)"), &p.position.x, 0.05f);
        ImGui::DragFloat3(tr("Direção", "Direction"), &p.direction.x, 0.05f);
        ImGui::SliderFloat(tr("Cone (rad)", "Cone (rad)"), &p.coneAngle, 0.0f, 1.5f);
        ImGui::DragFloat(tr("Taxa (part/s)", "Rate (part/s)"), &p.rate, 1.0f, 0.0f, 10000.0f);
        ImGui::DragFloat(tr("Vel. min", "Speed Min"), &p.speedMin, 0.1f, 0.0f, 100.0f);
        ImGui::DragFloat(tr("Vel. máx", "Speed Max"), &p.speedMax, 0.1f, 0.0f, 100.0f);
        ImGui::DragFloat(tr("Vida min (s)", "Lifetime Min"), &p.lifetimeMin, 0.05f, 0.01f, 60.0f);
        ImGui::DragFloat(tr("Vida máx (s)", "Lifetime Max"), &p.lifetimeMax, 0.05f, 0.01f, 60.0f);
        ImGui::DragFloat(tr("Tamanho inicial", "Size Start"), &p.sizeStart, 0.01f, 0.0f, 10.0f);
        ImGui::DragFloat(tr("Tamanho final", "Size End"), &p.sizeEnd, 0.01f, 0.0f, 10.0f);
        ImGui::ColorEdit4(tr("Cor inicial", "Start Color"), &p.colorStart.x);
        ImGui::ColorEdit4(tr("Cor final", "End Color"), &p.colorEnd.x);
        ImGui::DragFloat3(tr("Aceleração", "Acceleration"), &p.acceleration.x, 0.1f);
        ImGui::DragFloat(tr("Arrasto", "Drag"), &p.drag, 0.01f, 0.0f, 1.0f);
        ImGui::DragInt(tr("Rajada no início", "Burst on start"), reinterpret_cast<int*>(&p.burstCount), 1, 0, 100000);
        ImGui::Checkbox(tr("Colide com física", "Collides with physics"), &p.collide);
        ImGui::Checkbox(tr("Emitindo", "Emitting"), &p.emitting);
        if (ImGui::Button(tr("Remover Emissor", "Remove Emitter"))) scene->particleEmitterComponents.erase(id);
        ImGui::Spacing();
    }

    // Vehicle Component (authored in the Vehicle panel; the play world builds
    // a chassis body + four wheels and drives it with a VehicleRuntime).
    if (scene->vehicleComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.30f, 1.0f), "%s", tr("Veículo", "Vehicle Component"));
        ImGui::Separator();
        auto& v = scene->vehicleComponents[id];
        ImGui::DragFloat(tr("Potência do motor", "Engine Power"), &v.enginePower, 100.0f, 0.0f, 100000.0f);
        ImGui::SliderFloat(tr("Ângulo máx. de direção (rad)", "Max Steer Angle"), &v.maxSteerAngle, 0.0f, 1.2f);
        ImGui::DragFloat(tr("Força de freio", "Brake Force"), &v.brakeForce, 100.0f, 0.0f, 100000.0f);
        ImGui::DragFloat(tr("Raio da roda", "Wheel Radius"), &v.wheelRadius, 0.01f, 0.05f, 2.0f);
        ImGui::DragFloat(tr("Suspensão (descanso)", "Suspension Rest"), &v.suspensionRest, 0.01f, 0.0f, 2.0f);
        ImGui::DragFloat(tr("Distância entre eixos", "Wheel Base"), &v.wheelBase, 0.05f, 0.5f, 20.0f);
        ImGui::DragFloat(tr("Bitola (largura)", "Track Width"), &v.trackWidth, 0.05f, 0.2f, 10.0f);
        ImGui::DragFloat(tr("Massa", "Mass"), &v.mass, 50.0f, 10.0f, 20000.0f);
        ImGui::Checkbox(tr("Tração dianteira", "Front Wheel Drive"), &v.frontWheelDrive);
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &v.enabled);
        if (ImGui::Button(tr("Remover Veículo", "Remove Vehicle"))) scene->vehicleComponents.erase(id);
        ImGui::Spacing();
    }

    // Ragdoll Component (authored in the Ragdoll panel; the play world builds
    // physics bodies per bone from the skin skeleton when fromSkeleton is set).
    if (scene->ragdollComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.35f, 1.0f), "%s", tr("Ragdoll", "Ragdoll Component"));
        ImGui::Separator();
        auto& rg = scene->ragdollComponents[id];
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &rg.enabled);
        ImGui::SliderFloat(tr("Blend da física", "Physics Blend"), &rg.blendWeight, 0.0f, 1.0f);
        ImGui::Checkbox(tr("Da esqueleto (skin)", "From skeleton (skin)"), &rg.fromSkeleton);
        ImGui::DragFloat(tr("Massa por osso", "Mass per bone"), &rg.massPerBone, 0.1f, 0.1f, 100.0f);
        ImGui::DragFloat3(tr("Deslocamento de spawn", "Spawn Offset"), &rg.spawnOffset.x, 0.1f);
        if (ImGui::Button(tr("Remover Ragdoll", "Remove Ragdoll"))) scene->ragdollComponents.erase(id);
        ImGui::Spacing();
    }

    // Mission Component (the play world registers a Mission that the
    // completeEvent — a script EmitEvent — finishes).
    if (scene->missionComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s", tr("Missão", "Mission Component"));
        ImGui::Separator();
        auto& m = scene->missionComponents[id];
        char missionBuf[128]; std::snprintf(missionBuf, sizeof(missionBuf), "%s", m.missionId.c_str());
        if (ImGui::InputText(tr("ID", "ID"), missionBuf, sizeof(missionBuf))) m.missionId = missionBuf;
        char objBuf[256]; std::snprintf(objBuf, sizeof(objBuf), "%s", m.objectiveText.c_str());
        if (ImGui::InputText(tr("Objetivo", "Objective"), objBuf, sizeof(objBuf))) m.objectiveText = objBuf;
        ImGui::DragInt(tr("Alvo", "Target"), reinterpret_cast<int*>(&m.objectiveTarget), 1, 1, 100000);
        char evBuf[128]; std::snprintf(evBuf, sizeof(evBuf), "%s", m.completeEvent.c_str());
        if (ImGui::InputText(tr("Evento de conclusão", "Complete Event"), evBuf, sizeof(evBuf))) m.completeEvent = evBuf;
        ImGui::Checkbox(tr("Início automático", "Auto Start"), &m.autoStart);
        ImGui::TextDisabled("%s: %s", tr("Estado", "State"), m.active ? tr("ativa", "active") : tr("inativa", "inactive"));
        if (ImGui::Button(tr("Remover Missão", "Remove Mission"))) scene->missionComponents.erase(id);
        ImGui::Spacing();
    }

    // Dialogue Component (a one-node graph with a line and one choice).
    if (scene->dialogueComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.95f, 1.0f), "%s", tr("Diálogo", "Dialogue Component"));
        ImGui::Separator();
        auto& d = scene->dialogueComponents[id];
        char dgBuf[128]; std::snprintf(dgBuf, sizeof(dgBuf), "%s", d.dialogueId.c_str());
        if (ImGui::InputText("ID", dgBuf, sizeof(dgBuf))) d.dialogueId = dgBuf;
        char chBuf[128]; std::snprintf(chBuf, sizeof(chBuf), "%s", d.character.c_str());
        if (ImGui::InputText(tr("Personagem", "Character"), chBuf, sizeof(chBuf))) d.character = chBuf;
        char lineBuf[256]; std::snprintf(lineBuf, sizeof(lineBuf), "%s", d.line.c_str());
        if (ImGui::InputText(tr("Fala", "Line"), lineBuf, sizeof(lineBuf))) d.line = lineBuf;
        char choiceBuf[128]; std::snprintf(choiceBuf, sizeof(choiceBuf), "%s", d.choiceText.c_str());
        if (ImGui::InputText(tr("Escolha", "Choice"), choiceBuf, sizeof(choiceBuf))) d.choiceText = choiceBuf;
        char nextBuf[128]; std::snprintf(nextBuf, sizeof(nextBuf), "%s", d.nextDialogueId.c_str());
        if (ImGui::InputText(tr("Próximo diálogo", "Next Dialogue"), nextBuf, sizeof(nextBuf))) d.nextDialogueId = nextBuf;
        ImGui::Checkbox(tr("Tocar ao iniciar", "Play On Start"), &d.playOnStart);
        if (ImGui::Button(tr("Remover Diálogo", "Remove Dialogue"))) scene->dialogueComponents.erase(id);
        ImGui::Spacing();
    }

    // Destruction Component (a destructible of chunkCount boxes; weapon hits
    // within damageRadius detach chunks in play).
    if (scene->destructionComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.3f, 1.0f), "%s", tr("Destrutível", "Destruction Component"));
        ImGui::Separator();
        auto& ds = scene->destructionComponents[id];
        ImGui::DragFloat3(tr("Tamanho do pedaço", "Chunk Size"), &ds.chunkSize.x, 0.05f, 0.05f, 10.0f);
        ImGui::DragInt(tr("Nº de pedaços", "Chunk Count"), reinterpret_cast<int*>(&ds.chunkCount), 1, 1, 1000);
        ImGui::DragFloat(tr("Vida do pedaço", "Chunk Health"), &ds.chunkHealth, 1.0f, 1.0f, 100000.0f);
        ImGui::DragFloat(tr("Raio de dano", "Damage Radius"), &ds.damageRadius, 0.1f, 0.1f, 100.0f);
        ImGui::DragFloat(tr("Impulso do dano", "Damage Impulse"), &ds.damageImpulse, 0.5f, 0.0f, 1000.0f);
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &ds.enabled);
        if (ImGui::Button(tr("Remover Destrutível", "Remove Destruction"))) scene->destructionComponents.erase(id);
        ImGui::Spacing();
    }

    // Navigation Component (a baked grid + an agent toward the camera).
    if (scene->navigationComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.5f, 0.95f, 0.85f, 1.0f), "%s", tr("Navegação", "Navigation Component"));
        ImGui::Separator();
        auto& nav = scene->navigationComponents[id];
        ImGui::DragInt(tr("Largura do grid", "Grid Width"), &nav.gridWidth, 1, 4, 512);
        ImGui::DragInt(tr("Altura do grid", "Grid Height"), &nav.gridHeight, 1, 4, 512);
        ImGui::DragFloat(tr("Tamanho da célula", "Cell Size"), &nav.cellSize, 0.1f, 0.1f, 20.0f);
        ImGui::DragFloat(tr("Velocidade do agente", "Agent Speed"), &nav.agentSpeed, 0.1f, 0.1f, 50.0f);
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &nav.enabled);
        if (ImGui::Button(tr("Remover Navegação", "Remove Navigation"))) scene->navigationComponents.erase(id);
        ImGui::Spacing();
    }

    // Audio Component (an OGG source played through the play Mixer).
    if (scene->audioComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "%s", tr("Fonte de Áudio", "Audio Component"));
        ImGui::Separator();
        auto& au = scene->audioComponents[id];
        char clipBuf[256]; std::snprintf(clipBuf, sizeof(clipBuf), "%s", au.clipPath.c_str());
        if (ImGui::InputText(".ogg", clipBuf, sizeof(clipBuf))) au.clipPath = clipBuf;
        ImGui::DragFloat(tr("Volume", "Volume"), &au.volume, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat(tr("Pitch", "Pitch"), &au.pitch, 0.01f, 0.1f, 4.0f);
        ImGui::Checkbox(tr("Espacial", "Spatial"), &au.spatial);
        ImGui::SameLine();
        ImGui::Checkbox(tr("Em loop", "Looping"), &au.looping);
        ImGui::Checkbox(tr("Tocar ao iniciar", "Play On Start"), &au.playOnStart);
        if (ImGui::Button(tr("Remover Áudio", "Remove Audio"))) scene->audioComponents.erase(id);
        ImGui::Spacing();
    }

    // Mesh Renderer Component
    if (scene->meshRendererComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.55f, 0.45f, 0.98f, 1.0f), "%s", tr("Renderizador de Malha", "Mesh Renderer Component"));
        ImGui::Separator();
        auto& mr = scene->meshRendererComponents[id];
        // Mesh asset picker (from the project asset registry).
        std::vector<std::pair<UUID, std::string>> meshAssets;
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.type == AssetType::Mesh) {
                meshAssets.emplace_back(asset.id, asset.sourcePath.filename().string());
            }
        }
        const std::string noneLabel = tr("(Nenhuma malha)", "(None)");
        const char* currentName = noneLabel.c_str();
        int currentIndex = -1;
        for (size_t i = 0; i < meshAssets.size(); ++i) {
            if (meshAssets[i].first == mr.meshAssetID) {
                currentIndex = static_cast<int>(i);
                currentName = meshAssets[i].second.c_str();
                break;
            }
        }
        if (ImGui::BeginCombo(tr("Malha 3D", "Mesh"), currentName)) {
            if (ImGui::Selectable(noneLabel.c_str(), currentIndex < 0)) {
                mr.meshAssetID = UUID();
                m_meshLoadFailed.erase(UUID());
            }
            for (size_t i = 0; i < meshAssets.size(); ++i) {
                if (ImGui::Selectable(meshAssets[i].second.c_str(), currentIndex == static_cast<int>(i))) {
                    mr.meshAssetID = meshAssets[i].first;
                }
            }
            ImGui::EndCombo();
        }
        // Material asset picker: rendered on the mesh via a material-graph pipeline.
        std::vector<std::pair<UUID, std::string>> materialAssets;
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.type == AssetType::Material) {
                materialAssets.emplace_back(asset.id, asset.sourcePath.filename().string());
            }
        }
        const std::string matNoneLabel = tr("(Padrão)", "(Default)");
        const char* matCurrentName = matNoneLabel.c_str();
        int matCurrentIndex = -1;
        for (size_t i = 0; i < materialAssets.size(); ++i) {
            if (materialAssets[i].first == mr.materialAssetID) {
                matCurrentIndex = static_cast<int>(i);
                matCurrentName = materialAssets[i].second.c_str();
                break;
            }
        }
        if (ImGui::BeginCombo(tr("Material", "Material"), matCurrentName)) {
            if (ImGui::Selectable(matNoneLabel.c_str(), matCurrentIndex < 0)) {
                mr.materialAssetID = UUID();
                m_materialLoadFailed.erase(UUID());
            }
            for (size_t i = 0; i < materialAssets.size(); ++i) {
                if (ImGui::Selectable(materialAssets[i].second.c_str(), matCurrentIndex == static_cast<int>(i))) {
                    mr.materialAssetID = materialAssets[i].first;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox(tr("Visível", "Visible"), &mr.isVisible);
        ImGui::Checkbox(tr("Projetar Sombras", "Cast Shadows"), &mr.castShadows);
        ImGui::Spacing();
    }

#if VC_ENABLE_VOXEL_PLUGIN
    // Voxel Volume Component
    if (scene->voxelVolumeComponents.contains(id)) {
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("Mundo de Terreno em Blocos", "Voxel Terrain Volume"));
        ImGui::Separator();
        auto& v = scene->voxelVolumeComponents[id];
        ImGui::SliderInt(tr("Distância de Visão (Blocos)", "Chunk Radius"), &v.chunkBudget, 64, 4096);
        ImGui::InputInt(tr("Semente de Geração (Seed)", "Terrain Seed"), &v.seed);
        ImGui::DragFloat(tr("Nível da Água", "Sea Level"), &v.seaLevel, 0.5f, 0.0f, 100.0f);
        ImGui::Checkbox(tr("Carregar Terreno Distante", "Enable Far LOD Clipmap"), &v.enableFarLod);
        ImGui::Spacing();
    }
#endif

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.39f, 0.40f, 0.95f, 1.00f));
    if (ImGui::Button(tr("+ Adicionar Nova Propriedade", "+ Add Component"), ImVec2(240, 32))) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    ImGui::PopStyleColor();

    if (ImGui::BeginPopup("AddComponentPopup")) {
        if (ImGui::MenuItem(tr("Iluminação e Luz", "Light Component"))) scene->lightComponents[id] = LightComponent{};
        if (ImGui::MenuItem(tr("Câmera de Visão", "Camera Component"))) scene->cameraComponents[id] = CameraComponent{};
        if (ImGui::MenuItem(tr("Física e Gravidade", "Rigidbody Component"))) scene->rigidbodyComponents[id] = RigidbodyComponent{};
        if (ImGui::MenuItem(tr("Arma (Hitscan)", "Weapon Component"))) scene->weaponComponents[id] = WeaponComponent{};
        if (ImGui::MenuItem(tr("Emissor de Partículas", "Particle Emitter Component"))) scene->particleEmitterComponents[id] = ParticleEmitterComponent{};
        if (ImGui::MenuItem(tr("Veículo", "Vehicle Component"))) scene->vehicleComponents[id] = VehicleComponent{};
        if (ImGui::MenuItem(tr("Ragdoll", "Ragdoll Component"))) scene->ragdollComponents[id] = RagdollComponent{};
        if (ImGui::MenuItem(tr("Missão", "Mission Component"))) scene->missionComponents[id] = MissionComponent{};
        if (ImGui::MenuItem(tr("Diálogo", "Dialogue Component"))) scene->dialogueComponents[id] = DialogueComponent{};
        if (ImGui::MenuItem(tr("Destrutível", "Destruction Component"))) scene->destructionComponents[id] = DestructionComponent{};
        if (ImGui::MenuItem(tr("Navegação", "Navigation Component"))) scene->navigationComponents[id] = NavigationComponent{};
        if (ImGui::MenuItem(tr("Fonte de Áudio", "Audio Component"))) scene->audioComponents[id] = AudioComponent{};
        if (ImGui::MenuItem(tr("Modelo 3D (Mesh)", "Mesh Renderer"))) scene->meshRendererComponents[id] = MeshRendererComponent{};
#if VC_ENABLE_VOXEL_PLUGIN
        if (ImGui::MenuItem(tr("Mundo de Blocos", "Voxel Terrain Volume"))) scene->voxelVolumeComponents[id] = VoxelVolumeComponent{};
#endif
        ImGui::EndPopup();
    }

    ImGui::End();
}

void EditorApplication::draw_viewport_panel() {
    ImGui::Begin(tr("Visualização 3D", "Scene Viewport"));
    const ImVec2 panelSize = ImGui::GetContentRegionAvail();

    // Viewport Top Toolbar Info
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", tr("Visualização 3D em Tempo Real", "Real-Time 3D Preview")); ImGui::SameLine();
    ImGui::TextDisabled(" |  %s: %dx%d", tr("Resolução", "Resolution"), static_cast<int>(panelSize.x), static_cast<int>(panelSize.y));
    ImGui::TextDisabled("  |  W/E/R: %s", tr("Mover/Rotar/Escalar", "Translate/Rotate/Scale"));
    ImGui::SameLine();
    ImGui::TextDisabled("  |  Ctrl: %s", tr("Snap", "Snap"));
    ImGui::Separator();

    m_viewportHovered = ImGui::IsWindowHovered();
    m_viewportFocused = ImGui::IsWindowFocused();

    if (m_offscreen.imguiTextureID == VK_NULL_HANDLE) {
        ImGui::TextDisabled("%s", tr("O viewport 3D será criado ao entrar no editor...", "3D viewport is being prepared..."));
        ImGui::End();
        return;
    }

    // Fit the offscreen texture into the panel, preserving aspect ratio.
    const float texAspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    float dispW = std::max(1.0f, panelSize.x);
    float dispH = dispW / texAspect;
    if (dispH > std::max(1.0f, panelSize.y)) {
        dispH = std::max(1.0f, panelSize.y);
        dispW = dispH * texAspect;
    }
    const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    const ImVec2 dispPos(cursorPos.x + std::max(0.0f, (panelSize.x - dispW) * 0.5f), cursorPos.y);
    m_viewportImagePos = dispPos;
    m_viewportImageSize = ImVec2(dispW, dispH);
    m_viewportImageHovered = ImGui::IsMouseHoveringRect(dispPos, ImVec2(dispPos.x + dispW, dispPos.y + dispH));

    ImGui::SetCursorScreenPos(dispPos);
    // Vulkan images are top-down; flip V so the world appears upright.
    ImGui::Image(m_offscreen.imguiTextureID, ImVec2(dispW, dispH), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
#ifdef _WIN32
    // The editor render target may be square, but an embedded running game is
    // a native swapchain and must occupy the complete Scene Viewport panel.
    // Keeping it tied to dispW/dispH left half of widescreen panels uncovered.
    update_embedded_game(m_window, dispPos,
                         ImVec2(std::max(1.0f, panelSize.x),
                                std::max(1.0f, panelSize.y)));
#endif

    // Drag & drop de assets (Content Browser → cena): mesh cria uma entidade
    // em frente à câmera; material aplica na entidade selecionada.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_ASSET_UUID")) {
            if (payload->DataSize > 1) {
                const std::string droppedId(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                handle_asset_drop(UUID::from_string(droppedId));
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);

    if (m_viewportImageHovered) {
        // Left click: grab the gizmo axis first, otherwise pick the entity.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_gizmoDragging) {
            if (gizmo_axis_hit_test(mouse)) {
                m_activeAxis = m_hoveredAxis;
                start_gizmo_drag(mouse);
            } else {
                m_activeAxis = GizmoAxis::None;
                m_pickPixel = (mouse - glm::vec2(dispPos.x, dispPos.y)) *
                    glm::vec2(static_cast<float>(m_offscreen.width) / dispW,
                              static_cast<float>(m_offscreen.height) / dispH);
                m_pickRequested = true;
            }
        }
    }

    if (m_gizmoDragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            update_gizmo_drag(mouse);
        } else {
            m_gizmoDragging = false;
            m_activeAxis = GizmoAxis::None;
        }
    } else if (m_viewportImageHovered && !ImGui::IsAnyMouseDown()) {
        gizmo_axis_hit_test(mouse); // hover highlight
    }

    ImGui::End();
}

void EditorApplication::handle_asset_drop(const UUID& assetId) {
    Scene* scene = m_editorScene.get();
    if (!scene) return;
    const auto found = m_assetRegistry.find(assetId);
    if (!found) {
        std::cerr << "[Viewport] Dropped unknown asset " << assetId.to_string() << std::endl;
        return;
    }
    const AssetMetadata& asset = *found;
    if (asset.type == AssetType::Mesh) {
        Entity ent = scene->create_entity(asset.sourcePath.stem().string());
        scene->meshRendererComponents[ent.get_id()] = MeshRendererComponent{ asset.id, {}, true, true };
        scene->transformComponents[ent.get_id()].position =
            m_editorCamera.position + m_editorCamera.get_front() * 2.0f;
        m_selectedEntity = ent;
        std::cout << "[Viewport] Dropped mesh '" << asset.sourcePath.filename().string()
                  << "' -> spawned entity '" << ent.get_name() << "'\n";
    } else if (asset.type == AssetType::Material) {
        if (m_selectedEntity.is_valid()) {
            const auto it = scene->meshRendererComponents.find(m_selectedEntity.get_id());
            if (it != scene->meshRendererComponents.end()) {
                it->second.materialAssetID = asset.id;
                std::cout << "[Viewport] Dropped material '" << asset.sourcePath.filename().string()
                          << "' on '" << m_selectedEntity.get_name() << "'\n";
                return;
            }
        }
        std::cout << "[Viewport] Material drop needs a mesh entity selected\n";
    }
}

void EditorApplication::draw_content_browser_panel() {
    ImGui::Begin(tr("Arquivos do Projeto", "Project Content Browser"));

    static bool indexed = false;
    static char search[256]{};
    static int typeFilter = 0;
    static std::optional<UUID> selectedAssetId;
    static ImportSettings editedImportSettings;
    const std::filesystem::path projectAssets =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Projects" / m_currentProjectName / "Assets";
    const std::filesystem::path fallbackAssets = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets";
    const std::filesystem::path sourceRoot = std::filesystem::exists(projectAssets) ? projectAssets : fallbackAssets;
    const std::filesystem::path cookedRoot =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";

    if (!indexed && m_assetPipeline && std::filesystem::exists(sourceRoot)) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(sourceRoot, error), end; it != end && !error; it.increment(error)) {
            if (!it->is_regular_file()) continue;
            const ImportResult result = m_assetPipeline->import({it->path(), cookedRoot, 1});
            if (!result && result.error.rfind("No importer supports", 0) != 0) {
                std::cerr << "[ContentBrowser] " << result.error << std::endl;
            }
        }
        m_assetHotReload->watch_registered_assets();
        const std::filesystem::path registryPath =
            std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
        if (!m_assetRegistry.save(registryPath))
            std::cerr << "[AssetRegistry] Could not persist database: " << registryPath << std::endl;
        indexed = true;
    }

    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s: %s",
        tr("Pasta do Jogo", "Game Directory"), sourceRoot.string().c_str());
    ImGui::InputTextWithHint("##AssetSearch", tr("Pesquisar assets...", "Search assets..."), search, sizeof(search));
    ImGui::SameLine();
    const char* filters[] = { "All", "Texture", "Mesh", "Material", "Audio", "Scene", "Animation", "Unused" };
    ImGui::Combo("##AssetType", &typeFilter, filters, IM_ARRAYSIZE(filters));
    ImGui::SameLine();
    if (ImGui::Button(tr("Empacotar", "Package"))) {
        std::vector<UUID> roots;
        for (const AssetMetadata& candidate : m_assetRegistry.snapshot())
            if (candidate.type == AssetType::Scene) roots.push_back(candidate.id);
        const std::filesystem::path packageOutput = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
            "Packages" / m_currentProjectName;
        const AssetPackageResult packaged = AssetPackager::package(m_assetRegistry, roots, packageOutput);
        if (!packaged) std::cerr << "[AssetPackager] " << packaged.error << std::endl;
        else std::cout << "[AssetPackager] Packaged " << packaged.assets.size()
                       << " assets to " << packageOutput << std::endl;
    }
    ImGui::Separator();

    std::optional<AssetType> selectedType;
    switch (typeFilter) {
        case 1: selectedType = AssetType::Texture; break;
        case 2: selectedType = AssetType::Mesh; break;
        case 3: selectedType = AssetType::Material; break;
        case 4: selectedType = AssetType::Audio; break;
        case 5: selectedType = AssetType::Scene; break;
        case 6: selectedType = AssetType::Animation; break;
        default: break;
    }

    AssetBrowserModel browser(m_assetRegistry);
    std::vector<AssetMetadata> assets = browser.query(search, selectedType);
    if (typeFilter == 7) {
        std::vector<UUID> roots;
        for (const AssetMetadata& candidate : m_assetRegistry.snapshot())
            if (candidate.type == AssetType::Scene) roots.push_back(candidate.id);
        const std::vector<UUID> unused = m_assetRegistry.unused_assets(roots);
        const std::unordered_set<UUID> unusedSet(unused.begin(), unused.end());
        assets.erase(std::remove_if(assets.begin(), assets.end(), [&](const AssetMetadata& candidate) {
            return !unusedSet.contains(candidate.id);
        }), assets.end());
    }
    const float cellSize = 150.0f;
    int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellSize));
    ImGui::Columns(columns, "AssetGrid", false);
    for (const AssetMetadata& asset : assets) {
        const std::string filename = asset.sourcePath.filename().string();
        ImGui::PushID(asset.id.to_string().c_str());
        if (ImGui::Button(filename.c_str(), ImVec2(135, 65))) {
            selectedAssetId = asset.id;
            editedImportSettings = asset.importSettings;
        }
        if (ImGui::BeginPopupContextItem("AssetContext")) {
            if (ImGui::MenuItem(tr("Duplicar", "Duplicate"))) {
                std::filesystem::path duplicatePath = asset.sourcePath.parent_path() /
                    (asset.sourcePath.stem().string() + "_copy" + asset.sourcePath.extension().string());
                unsigned suffix = 2;
                while (std::filesystem::exists(duplicatePath)) {
                    duplicatePath = asset.sourcePath.parent_path() /
                        (asset.sourcePath.stem().string() + "_copy" + std::to_string(suffix++) + asset.sourcePath.extension().string());
                }
                const AssetFileOperationResult duplicated = browser.duplicate_asset(asset.id, duplicatePath);
                if (!duplicated) {
                    std::cerr << "[ContentBrowser] " << duplicated.error << std::endl;
                } else {
                    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
                        "Intermediate" / "AssetRegistry.db";
                    if (!m_assetRegistry.save(registryPath))
                        std::cerr << "[AssetRegistry] Could not persist duplicated asset" << std::endl;
                }
            }
            if (ImGui::MenuItem(tr("Excluir", "Delete"))) {
                const AssetFileOperationResult deleted = browser.delete_asset(asset.id);
                if (!deleted) {
                    std::cerr << "[ContentBrowser] " << deleted.error << std::endl;
                } else {
                    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
                        "Intermediate" / "AssetRegistry.db";
                    if (!m_assetRegistry.save(registryPath))
                        std::cerr << "[AssetRegistry] Could not persist asset deletion" << std::endl;
                }
            }
            const auto referencers = m_assetRegistry.referencers_of(asset.id);
            if (!referencers.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("%zu reference(s)", referencers.size());
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropSource()) {
            const std::string id = asset.id.to_string();
            ImGui::SetDragDropPayload("CONTENT_ASSET_UUID", id.c_str(), id.size() + 1);
            ImGui::TextUnformatted(filename.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::TextDisabled("%s", asset.isCooked ? "Cooked" : "Source");
        ImGui::TextWrapped("%s", filename.c_str());
        ImGui::PopID();
        ImGui::NextColumn();
    }
    ImGui::Columns(1);

    if (selectedAssetId) {
        const auto selected = m_assetRegistry.find(*selectedAssetId);
        if (!selected) {
            selectedAssetId.reset();
        } else {
            ImGui::SeparatorText(tr("Configurações de Importação", "Import Settings"));
            ImGui::Text("%s", selected->sourcePath.filename().string().c_str());
            ImGui::TextDisabled("UUID: %s", selected->id.to_string().c_str());
            ImGui::TextDisabled("Cooked: %s", selected->cookedPath.string().c_str());
            if (selected->type == AssetType::Texture) {
                ImGui::Checkbox(tr("Gerar mipmaps", "Generate mipmaps"), &editedImportSettings.generateMipmaps);
                ImGui::Checkbox("sRGB", &editedImportSettings.srgb);
                int textureQuality = static_cast<int>(editedImportSettings.textureQuality);
                if (ImGui::SliderInt(tr("Qualidade", "Quality"), &textureQuality, 0, 100))
                    editedImportSettings.textureQuality = static_cast<uint32_t>(textureQuality);
                ImGui::TextDisabled("%u x %u, %u channel(s)", selected->width, selected->height, selected->channels);
            } else if (selected->type == AssetType::Mesh) {
                ImGui::DragFloat(tr("Escala da mesh", "Mesh scale"), &editedImportSettings.meshScale,
                                 0.01f, 0.001f, 1000.0f, "%.3f");
                ImGui::TextDisabled("%u primitive(s), %llu vertices, %llu indices",
                    selected->primitiveCount,
                    static_cast<unsigned long long>(selected->vertexCount),
                    static_cast<unsigned long long>(selected->indexCount));
            } else if (selected->type == AssetType::Audio) {
                ImGui::TextDisabled("%u Hz, %u channel(s), %.2f s", selected->sampleRate,
                                    selected->audioChannels, selected->durationSeconds);
            } else {
                ImGui::TextDisabled("No editable import settings for this asset type");
            }
            const bool editable = selected->type == AssetType::Texture || selected->type == AssetType::Mesh;
            if (!editable) ImGui::BeginDisabled();
            if (ImGui::Button(tr("Aplicar e reimportar", "Apply and reimport"))) {
                const ImportResult reimported = m_assetPipeline->import({
                    .source = selected->sourcePath,
                    .cookedDirectory = cookedRoot,
                    .importerVersion = selected->importerVersion,
                    .settings = editedImportSettings});
                if (!reimported) {
                    std::cerr << "[ContentBrowser] Reimport failed: " << reimported.error << std::endl;
                } else {
                    editedImportSettings = reimported.asset.importSettings;
                    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
                        "Intermediate" / "AssetRegistry.db";
                    if (!m_assetRegistry.save(registryPath))
                        std::cerr << "[AssetRegistry] Could not persist import settings" << std::endl;
                    if (m_assetHotReload) m_assetHotReload->watch_registered_assets();
                }
            }
            if (!editable) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(tr("Fechar", "Close"))) selectedAssetId.reset();
            const auto dependencies = m_assetRegistry.dependencies_of(selected->id);
            const auto referencers = m_assetRegistry.referencers_of(selected->id);
            ImGui::TextDisabled("%zu dependencies, %zu referencers", dependencies.size(), referencers.size());
        }
    }

    if (m_assetHotReload) {
        const auto reloaded = m_assetHotReload->poll();
        if (!reloaded.empty()) ImGui::Text("%zu asset(s) reimported", reloaded.size());
    }
    ImGui::End();
}

void EditorApplication::load_script_canvas() {
    m_scriptCanvasPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Content" / "Scenes" / "Initial.script";
    m_scriptCanvas = VisualScriptCanvas{};
    m_scriptCanvasLoaded = true;
    if (!std::filesystem::exists(m_scriptCanvasPath)) {
        std::cout << "[Editor] Script Canvas: " << m_scriptCanvasPath.string() << " not found — starting empty\n";
        return;
    }
    ScriptGraphAsset asset;
    if (!asset.load(m_scriptCanvasPath)) {
        std::cerr << "[Editor] Script Canvas: failed to load " << m_scriptCanvasPath.string() << '\n';
        return;
    }
    m_scriptCanvas = VisualScriptCanvas(to_visual_graph(asset));
    // Stagger the layout so nodes never stack on top of each other.
    float x = 40.0f;
    for (const ScriptNode& node : m_scriptCanvas.nodes()) {
        m_scriptCanvas.move_node(node.id, glm::vec2(x, 60.0f));
        x += 200.0f;
    }
    std::cout << "[Editor] Script Canvas loaded: " << m_scriptCanvasPath.string()
              << " (nodes=" << m_scriptCanvas.nodes().size()
              << ", connections=" << m_scriptCanvas.connections().size() << ")\n";
}

void EditorApplication::save_script_canvas() {
    if (m_scriptCanvasPath.empty()) load_script_canvas();
    const ScriptGraphAsset asset = from_visual_graph(m_scriptCanvas.graph());
    if (asset.save(m_scriptCanvasPath)) {
        m_scriptCanvas.mark_saved();
        std::cout << "[Editor] Script Canvas saved: " << m_scriptCanvasPath.string()
                  << " (nodes=" << asset.nodes.size() << ", links=" << asset.links.size()
                  << ") — play mode hot-reloads it\n";
    } else {
        std::cerr << "[Editor] Script Canvas: save failed: " << m_scriptCanvasPath.string() << '\n';
    }
}

void EditorApplication::add_canvas_node(const std::string& kind, glm::vec2 worldPos) {
    ScriptNode node;
    node.id = UUID();
    node.title = kind;
    const auto pin = [](const std::string& name, PinType type, bool isInput) {
        ScriptPin p;
        p.id = UUID();
        p.name = name;
        p.type = type;
        p.isInput = isInput;
        return p;
    };
    if (kind == "Event" || kind == "Return" || kind == "Scope" || kind == "Scope End" ||
        kind == "Function" || kind == "Function Call" || kind == "Emit Event" || kind == "Wait") {
        if (kind != "Return" && kind != "Scope End") node.outputs.push_back(pin("Out", PinType::Execution, false));
        node.inputs.push_back(pin("In", PinType::Execution, true));
    } else if (kind == "Branch") {
        node.inputs.push_back(pin("In", PinType::Execution, true));
        node.inputs.push_back(pin("Condition", PinType::Boolean, true));
        node.outputs.push_back(pin("True", PinType::Execution, false));
        node.outputs.push_back(pin("False", PinType::Execution, false));
    } else if (kind == "Constant Float") {
        node.outputs.push_back(pin("Value", PinType::Float, false));
    } else if (kind == "Constant Integer") {
        node.outputs.push_back(pin("Value", PinType::Integer, false));
    } else if (kind == "Constant Boolean") {
        node.outputs.push_back(pin("Value", PinType::Boolean, false));
    } else if (kind == "Get Variable") {
        node.inputs.push_back(pin("In", PinType::Execution, true));
        node.outputs.push_back(pin("Value", PinType::Float, false));
    } else if (kind == "Set Variable") {
        node.inputs.push_back(pin("In", PinType::Execution, true));
        node.inputs.push_back(pin("Value", PinType::Float, true));
        node.outputs.push_back(pin("Out", PinType::Execution, false));
    } else if (kind == "Log") {
        node.inputs.push_back(pin("In", PinType::Execution, true));
        node.inputs.push_back(pin("Message", PinType::Float, true));
        node.outputs.push_back(pin("Out", PinType::Execution, false));
    } else if (kind == "Add Float" || kind == "Subtract Float" || kind == "Multiply Float") {
        node.inputs.push_back(pin("A", PinType::Float, true));
        node.inputs.push_back(pin("B", PinType::Float, true));
        node.outputs.push_back(pin("Result", PinType::Float, false));
    } else {
        return; // unknown kind
    }
    m_scriptCanvas.add_node(node, worldPos);
    m_scriptCanvas.clear_selection();
    m_scriptCanvas.select(node.id);
}

void EditorApplication::draw_script_canvas_panel() {
    if (!ImGui::Begin(tr("Canvas de Scripts", "Script Canvas"), &m_showScriptCanvas)) {
        ImGui::End();
        return;
    }

    // Toolbar.
    if (ImGui::Button(tr("Salvar", "Save"))) save_script_canvas();
    ImGui::SameLine();
    if (ImGui::Button(tr("Recarregar", "Reload"))) load_script_canvas();
    ImGui::SameLine();
    if (ImGui::Button(tr("Desfazer", "Undo"))) m_scriptCanvas.undo();
    ImGui::SameLine();
    if (ImGui::Button(tr("Refazer", "Redo"))) m_scriptCanvas.redo();
    ImGui::SameLine();
    if (ImGui::BeginCombo("##add", m_canvasAddKind.c_str())) {
        static const char* kinds[] = {"Event", "Constant Float", "Constant Integer", "Constant Boolean",
                                      "Get Variable", "Set Variable", "Add Float", "Subtract Float",
                                      "Multiply Float", "Branch", "Wait", "Emit Event", "Log",
                                      "Function", "Function Call", "Scope", "Scope End", "Return"};
        for (const char* kind : kinds) {
            if (ImGui::Selectable(kind, m_canvasAddKind == kind)) m_canvasAddKind = kind;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Adicionar", "Add"))) {
        add_canvas_node(m_canvasAddKind, m_scriptCanvas.screen_to_world(glm::vec2(60.0f, 40.0f)));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s: %zu nós, %zu ligações, zoom %.2f%s",
                        m_scriptCanvas.dirty() ? tr("sujo", "dirty") : tr("salvo", "saved"),
                        m_scriptCanvas.nodes().size(), m_scriptCanvas.connections().size(),
                        m_scriptCanvas.zoom(), m_scriptCanvas.can_undo() ? " [U/D disponível]" : "");
    for (const auto& issue : m_scriptCanvas.validate()) {
        if (issue.severity == CanvasIssue::Severity::Info) continue;
        const ImVec4 color = issue.severity == CanvasIssue::Severity::Error
                                 ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                                 : ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
        ImGui::TextColored(color, "[%s] %s: %s",
                           issue.severity == CanvasIssue::Severity::Error ? "erro" : "aviso",
                           issue.field.c_str(), issue.message.c_str());
    }

    // Canvas surface.
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x < 10.0f || canvasSize.y < 10.0f) { ImGui::End(); return; }
    ImGui::InvisibleButton("##scriptcanvas", canvasSize);
    const bool canvasHovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const glm::vec2 canvasOrigin(canvasPos.x, canvasPos.y);

    // Background + grid.
    draw->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                        IM_COL32(28, 28, 32, 255));
    const float gridStep = 24.0f * m_scriptCanvas.zoom();
    if (gridStep > 8.0f) {
        const glm::vec2 worldTopLeft = m_scriptCanvas.screen_to_world(glm::vec2(0.0f, 0.0f));
        const glm::vec2 worldBottomRight =
            m_scriptCanvas.screen_to_world(glm::vec2(canvasSize.x, canvasSize.y));
        for (float gx = std::floor(worldTopLeft.x) * gridStep; gx < worldBottomRight.x * m_scriptCanvas.zoom() + canvasPos.x; gx += gridStep) {
            draw->AddLine(ImVec2(canvasPos.x + gx, canvasPos.y),
                          ImVec2(canvasPos.x + gx, canvasPos.y + canvasSize.y), IM_COL32(45, 45, 52, 255));
        }
        for (float gy = std::floor(worldTopLeft.y) * gridStep; gy < worldBottomRight.y * m_scriptCanvas.zoom() + canvasPos.y; gy += gridStep) {
            draw->AddLine(ImVec2(canvasPos.x, canvasPos.y + gy),
                          ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + gy), IM_COL32(45, 45, 52, 255));
        }
    }

    // Pan (middle drag) and zoom (wheel around the cursor).
    if (canvasHovered) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            m_scriptCanvas.pan_by(glm::vec2(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y));
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const glm::vec2 before = m_scriptCanvas.screen_to_world(
                glm::vec2(ImGui::GetIO().MousePos.x - canvasPos.x, ImGui::GetIO().MousePos.y - canvasPos.y));
            m_scriptCanvas.set_zoom(m_scriptCanvas.zoom() * (wheel > 0.0f ? 1.15f : 1.0f / 1.15f));
            const glm::vec2 after = m_scriptCanvas.screen_to_world(
                glm::vec2(ImGui::GetIO().MousePos.x - canvasPos.x, ImGui::GetIO().MousePos.y - canvasPos.y));
            m_scriptCanvas.pan_by(glm::vec2((after.x - before.x) * m_scriptCanvas.zoom(),
                                            (after.y - before.y) * m_scriptCanvas.zoom()));
        }
    }

    // Pin world position: y offset by index within the node's pin list.
    const auto pin_pos = [&](const ScriptNode& node, const ScriptPin& pin, bool isInput) -> glm::vec2 {
        const CanvasRect rect = m_scriptCanvas.node_rect(node.id);
        int index = 0;
        if (isInput) {
            for (std::size_t i = 0; i < node.inputs.size(); ++i) if (node.inputs[i].id == pin.id) { index = static_cast<int>(i); break; }
        } else {
            for (std::size_t i = 0; i < node.outputs.size(); ++i) if (node.outputs[i].id == pin.id) { index = static_cast<int>(i); break; }
        }
        const glm::vec2 world(std::min(rect.min.x, rect.max.x) + 8.0f,
                              std::min(rect.min.y, rect.max.y) + 24.0f +
                                  static_cast<float>(index) * 18.0f);
        return m_scriptCanvas.world_to_screen(world) + canvasOrigin;
    };
    // Wires (behind nodes).
    const auto pinScreen = [&](UUID owner, UUID pin) -> std::optional<glm::vec2> {
        for (const ScriptNode& node : m_scriptCanvas.nodes()) {
            if (node.id != owner) continue;
            for (const ScriptPin& p : node.inputs) if (p.id == pin) return pin_pos(node, p, true);
            for (const ScriptPin& p : node.outputs) if (p.id == pin) return pin_pos(node, p, false);
        }
        return std::nullopt;
    };

    for (const ScriptConnection& connection : m_scriptCanvas.connections()) {
        const auto fromPos = pinScreen(connection.fromPinID, connection.fromPinID);
        const auto toPos = pinScreen(connection.toPinID, connection.toPinID);
        if (!fromPos || !toPos) continue;
        draw->AddBezierCubic(ImVec2(fromPos->x, fromPos->y),
                             ImVec2(fromPos->x + 60.0f, fromPos->y),
                             ImVec2(toPos->x - 60.0f, toPos->y),
                             ImVec2(toPos->x, toPos->y),
                             IM_COL32(140, 160, 220, 255), 2.0f);
    }
    // In-progress drag wire.
    if (m_canvasDragPin.is_valid()) {
        const ImVec2 mouse(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        draw->AddBezierCubic(ImVec2(m_canvasDragPinPos.x, m_canvasDragPinPos.y),
                             ImVec2(m_canvasDragPinPos.x + 60.0f, m_canvasDragPinPos.y),
                             ImVec2(mouse.x - 60.0f, mouse.y), mouse,
                             IM_COL32(220, 180, 80, 255), 2.0f);
    }

    // Nodes.
    for (const ScriptNode& node : m_scriptCanvas.nodes()) {
        const CanvasRect rect = m_scriptCanvas.node_rect(node.id);
        const glm::vec2 topLeft = m_scriptCanvas.world_to_screen(rect.min) + canvasOrigin;
        const glm::vec2 bottomRight = m_scriptCanvas.world_to_screen(rect.max) + canvasOrigin;
        const bool selected = m_scriptCanvas.is_selected(node.id);
        draw->AddRectFilled(ImVec2(topLeft.x, topLeft.y), ImVec2(bottomRight.x, bottomRight.y),
                            selected ? IM_COL32(58, 66, 92, 255) : IM_COL32(48, 50, 62, 255), 6.0f);
        draw->AddRect(ImVec2(topLeft.x, topLeft.y), ImVec2(bottomRight.x, bottomRight.y),
                      selected ? IM_COL32(110, 150, 255, 255) : IM_COL32(90, 95, 115, 255), 6.0f);
        draw->AddText(ImVec2(topLeft.x + 8.0f, topLeft.y + 4.0f), IM_COL32(230, 230, 235, 255), node.title.c_str());
        // Pins.
        const auto drawPin = [&](const ScriptPin& pin, bool isInput) {
            const glm::vec2 p = pin_pos(node, pin, isInput);
            const ImU32 color = pin.type == PinType::Execution ? IM_COL32(190, 120, 220, 255)
                                : (pin.type == PinType::Boolean ? IM_COL32(90, 200, 120, 255)
                                : (pin.type == PinType::Integer ? IM_COL32(120, 170, 240, 255)
                                : IM_COL32(240, 180, 90, 255)));
            draw->AddCircleFilled(ImVec2(p.x, p.y), 5.0f, color);
            draw->AddCircle(ImVec2(p.x, p.y), 5.0f, IM_COL32(20, 20, 25, 255));
            draw->AddText(ImVec2(p.x + (isInput ? 9.0f : -9.0f - ImGui::CalcTextSize(pin.name.c_str()).x), p.y - 7.0f),
                          IM_COL32(200, 200, 210, 255), pin.name.c_str());
        };
        for (const ScriptPin& pin : node.inputs) drawPin(pin, true);
        for (const ScriptPin& pin : node.outputs) drawPin(pin, false);
    }

    // Interaction: pick / drag nodes, drag pins to connect, marquee, delete.
    const glm::vec2 mouseWorld = m_scriptCanvas.screen_to_world(
        glm::vec2(ImGui::GetIO().MousePos.x - canvasPos.x, ImGui::GetIO().MousePos.y - canvasPos.y));
    const bool leftClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool leftRelease = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (canvasHovered && leftClick) {
        // Pin hit test first (connect start).
        bool pinHit = false;
        for (const ScriptNode& node : m_scriptCanvas.nodes()) {
            for (const ScriptPin& pin : node.inputs) {
                const glm::vec2 p = pin_pos(node, pin, true);
                if (glm::distance(p, glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y)) <= 8.0f) {
                    // Dropping an in-progress wire on an input pin.
                    if (m_canvasDragPin.is_valid()) {
                        std::string reason;
                        if (!m_scriptCanvas.connect(m_canvasDragPin, pin.id, &reason)) {
                            std::cerr << "[Script Canvas] connect: " << reason << '\n';
                        }
                        m_canvasDragPin = UUID{0, 0};
                    }
                    pinHit = true;
                    break;
                }
            }
            if (pinHit) break;
            for (const ScriptPin& pin : node.outputs) {
                const glm::vec2 p = pin_pos(node, pin, false);
                if (glm::distance(p, glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y)) <= 8.0f) {
                    if (m_canvasDragPin.is_valid()) {
                        std::string reason;
                        if (!m_scriptCanvas.connect(m_canvasDragPin, pin.id, &reason)) {
                            std::cerr << "[Script Canvas] connect: " << reason << '\n';
                        }
                        m_canvasDragPin = UUID{0, 0};
                    } else {
                        m_canvasDragPin = pin.id;
                        m_canvasDragPinPos = p;
                    }
                    pinHit = true;
                    break;
                }
            }
            if (pinHit) break;
        }
        if (!pinHit) {
            const UUID hit = m_scriptCanvas.node_at(mouseWorld);
            if (hit.is_valid()) {
                m_scriptCanvas.select(hit, ImGui::GetIO().KeyCtrl);
            } else if (!ImGui::GetIO().KeyCtrl) {
                m_scriptCanvas.clear_selection();
            }
        }
    }
    if (leftRelease && m_canvasDragPin.is_valid()) {
        // Dropped on empty space: keep the pin selected but clear the drag.
        m_canvasDragPin = UUID{0, 0};
    }
    // Drag selected nodes (left held after click on a node).
    if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        m_scriptCanvas.selection_count() > 0) {
        const UUID under = m_scriptCanvas.node_at(mouseWorld);
        if (!under.is_valid() || m_scriptCanvas.is_selected(under)) {
            m_scriptCanvas.move_selection(glm::vec2(ImGui::GetIO().MouseDelta.x / m_scriptCanvas.zoom(),
                                                    ImGui::GetIO().MouseDelta.y / m_scriptCanvas.zoom()));
        }
    }
    // Delete key removes the selection.
    if (canvasHovered && ImGui::IsKeyPressed(ImGuiKey_Delete) && m_scriptCanvas.selection_count() > 0) {
        m_scriptCanvas.begin_batch("delete");
        const auto selection = m_scriptCanvas.selection();
        for (const UUID& id : selection) m_scriptCanvas.remove_node(id);
        m_scriptCanvas.end_batch();
    }
    // Undo/redo shortcuts.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) m_scriptCanvas.undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) m_scriptCanvas.redo();

    ImGui::End();
}

void EditorApplication::draw_script_debugger_panel() {
    using namespace Engine::Scripting;
    ImGui::Begin(tr("Debugger de Scripts", "Script Debugger"));
    const bool playing = m_playMode.get_state() == PlayState::Play ||
                         m_playMode.get_state() == PlayState::Simulate;
    if (!m_playScriptLoaded || !playing) {
        ImGui::TextWrapped("%s", tr(
            "Inicie o Play para depurar Initial.script: breakpoints, passo a passo, variáveis e watches ao vivo.",
            "Start Play to debug Initial.script: breakpoints, stepping, live variables and watches."));
        ImGui::TextDisabled("%s", m_playScriptPath.string().c_str());
        ImGui::End();
        return;
    }

    const VMStatus vmStatus = m_playScript.status();
    const size_t ip = m_playScript.instruction_pointer();
    const std::string stateText =
        vmStatus == VMStatus::Paused   ? tr("Em pausa (breakpoint)", "Paused (breakpoint)") :
        vmStatus == VMStatus::Completed ? tr("Concluído", "Completed") :
        vmStatus == VMStatus::Error    ? ("Error: " + m_playScript.error()) :
        m_scriptPauseRequested         ? tr("Segurando", "Held") : tr("Executando", "Running");
    ImGui::Text("%s | ip=%zu", stateText.c_str(), ip);

    const bool pausedOrHeld = m_scriptPauseRequested || vmStatus == VMStatus::Paused;
    if (pausedOrHeld) {
        if (ImGui::Button(tr("Continuar", "Continue"))) {
            m_scriptPauseRequested = false;
            if (vmStatus == VMStatus::Paused) m_scriptDebugger.continue_run(10000, 0.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Passo", "Step"))) m_scriptDebugger.step_into(0.0f);
        ImGui::SameLine();
        if (ImGui::Button(tr("Pular", "Step Over"))) m_scriptDebugger.step_over(0.0f);
        ImGui::SameLine();
        if (ImGui::Button(tr("Sair", "Step Out"))) m_scriptDebugger.step_out(0.0f);
    } else {
        if (ImGui::Button(tr("Pausar", "Pause"))) m_scriptPauseRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Reiniciar", "Restart"))) {
        m_scriptPauseRequested = false;
        if (m_playScript.start_event("OnStart")) m_scriptDebugger.continue_run(10000, 0.0f);
    }

    // Compiled bytecode with click-to-toggle breakpoints; the current
    // instruction is highlighted. The executing node (sourceNode) is shown
    // so the user can correlate bytecode with the graph.
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Bytecode (clique para alternar breakpoint)", "Bytecode (click to toggle breakpoint)"));
    const ScriptProgram& prog = m_playScript.program();
    if (ImGui::BeginChild("##scriptInstr", ImVec2(0, 280), true)) {
        for (size_t i = 0; i < prog.instructions.size(); ++i) {
            const Instruction& inst = prog.instructions[i];
            const bool isBp = m_scriptDebugger.has_breakpoint(i);
            const bool isIp = (i == ip);
            std::string label = (isBp ? "[B] " : "    ") + std::to_string(i) + "  " +
                                script_opcode_name(inst.opcode);
            if (!inst.text.empty()) label += " '" + inst.text + "'";
            if (std::holds_alternative<double>(inst.operand))
                label += " " + std::to_string(std::get<double>(inst.operand));
            if (inst.target != 0) label += " ->" + std::to_string(inst.target);
            if (isIp) label += "   <<<";
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), isBp)) {
                if (isBp) m_scriptDebugger.remove_breakpoint(i);
                else m_scriptDebugger.add_breakpoint(i);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Graph view: the authored nodes, with the node owning the current
    // instruction highlighted.
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Nós do grafo", "Graph nodes"));
    UUID currentNode{ 0, 0 };
    if (ip < prog.instructions.size()) currentNode = prog.instructions[ip].sourceNode;
    if (ImGui::BeginChild("##scriptNodes", ImVec2(0, 110), true)) {
        for (const TypedScriptNode& node : m_scriptDebugGraph.nodes) {
            std::string label = script_node_kind_name(node.kind);
            if (!node.event.empty()) label += " '" + node.event + "'";
            if (!node.variable.empty()) label += " '" + node.variable + "'";
            const bool active = node.id == currentNode;
            if (active) label += "   <<<";
            ImGui::TextColored(active ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                               "%s", label.c_str());
        }
    }
    ImGui::EndChild();

    // Live variables.
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Variáveis", "Variables"));
    if (ImGui::BeginChild("##scriptVars", ImVec2(0, 110), true)) {
        if (m_playScript.variables().empty()) ImGui::TextDisabled("(sem variáveis)");
        for (const auto& [name, value] : m_playScript.variables())
            ImGui::Text("%s = %s", name.c_str(), ScriptDebugger::value_to_string(value).c_str());
    }
    ImGui::EndChild();

    // Call stack.
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Pilha de chamadas", "Call Stack"));
    const auto& frames = m_scriptDebugger.call_stack();
    if (frames.empty()) ImGui::TextDisabled("(frame principal)");
    for (const auto& frame : frames) ImGui::Text("%s @ %zu", frame.name.c_str(), frame.entry);

    // Watch expressions (evaluated against the current scope).
    ImGui::Separator();
    static char watchBuf[128] = "";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##watchExpr", watchBuf, sizeof(watchBuf));
    ImGui::SameLine();
    if (ImGui::Button(tr("Adicionar Watch", "Add Watch"))) {
        if (watchBuf[0]) {
            m_scriptDebugger.add_watch(watchBuf);
            watchBuf[0] = '\0';
        }
    }
    m_scriptDebugger.evaluate_watches();
    for (const auto& watch : m_scriptDebugger.watches())
        ImGui::Text("%s = %s", watch.expression.c_str(), watch.result.c_str());
    ImGui::End();
}

void EditorApplication::draw_voxel_tool_panel() {
#if VC_ENABLE_VOXEL_PLUGIN
    ImGui::Begin(tr("Escultura de Blocos", "Voxel Sculpting Tools"));

    static int shapeIdx = 0;
    const char* shapesPt[] = { "Esfera", "Cubo", "Cilindro", "Carimbo" };
    const char* shapesEn[] = { "Sphere", "Cube", "Cylinder", "Stamp" };
    ImGui::Combo(tr("Formato do Pincel", "Brush Shape"), &shapeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? shapesPt : shapesEn, 4);

    static int modeIdx = 0;
    const char* modesPt[] = { "Colocar Blocos", "Destruir Blocos", "Substituir Blocos", "Pintar Material" };
    const char* modesEn[] = { "Add Voxels", "Remove Voxels", "Replace Voxels", "Paint Material" };
    ImGui::Combo(tr("Modo de Ação", "Brush Mode"), &modeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? modesPt : modesEn, 4);

    ImGui::SliderFloat(tr("Tamanho do Pincel", "Brush Radius"), &m_activeVoxelBrush.radius, 0.5f, 25.0f);

    static int voxelTypeIdx = 1;
    const char* materialsPt[] = { "Ar", "Grama", "Terra", "Pedra", "Areia", "Madeira", "Vidro", "Pedregulho", "Obsidiana", "Basalto" };
    const char* materialsEn[] = { "Air", "Grass", "Dirt", "Stone", "Sand", "Wood", "Glass", "Cobblestone", "Obsidian", "Basalt" };
    ImGui::Combo(tr("Tipo de Bloco", "Voxel Material"), &voxelTypeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? materialsPt : materialsEn, 10);

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.82f, 0.60f, 1.00f));
    if (ImGui::Button(tr("Aplicar Pincel", "Apply Brush"), ImVec2(220, 32))) {
        m_activeVoxelBrush.shape = static_cast<VoxelBrushShape>(shapeIdx);
        m_activeVoxelBrush.mode = static_cast<VoxelBrushMode>(modeIdx);
        m_activeVoxelBrush.voxelType = static_cast<uint16_t>(voxelTypeIdx);
    }
    ImGui::PopStyleColor();

    ImGui::End();
#endif
}

void EditorApplication::run_game_build() {
    m_buildLog.clear();
    const auto log = [this](const std::string& line) {
        m_buildLog.push_back(line);
        std::cout << "[Build] " << line << std::endl;
    };
    const std::filesystem::path sourceRoot = std::filesystem::path(VULKANCRAFT_SOURCE_DIR);
    const std::filesystem::path cookedRoot = sourceRoot / "Intermediate" / "DerivedDataCache";
    const std::filesystem::path buildRoot =
        sourceRoot / "Projects" / m_currentProjectName / "Build" / m_currentProjectName;
    const std::filesystem::path binDir = buildRoot / "Bin";
    std::error_code ec;

    if (!m_editorScene || !m_assetPipeline) {
        log("Build failed: no scene open");
        return;
    }
    log("Build started for project '" + m_currentProjectName + "'");

    // 1. Cook every uncooked asset (same path as the Content Browser).
    size_t imported = 0, failed = 0;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) continue;
        const ImportResult result = m_assetPipeline->import({ asset.sourcePath, cookedRoot, 1 });
        if (result) ++imported;
        else { ++failed; log("  cook failed: " + result.error); }
    }
    log(std::to_string(imported) + " asset(s) cooked, " + std::to_string(failed) + " failed");

    // 2. Package all cooked assets (Content/<uuid>/<file> + AssetManifest.txt).
    std::vector<UUID> roots;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) roots.push_back(asset.id);
    }
    if (roots.empty()) {
        log("Build failed: no cooked assets to package");
        return;
    }
    const AssetPackageResult packaged = AssetPackager::package(m_assetRegistry, roots, buildRoot);
    if (!packaged) {
        log("Build failed: " + packaged.error);
        return;
    }
    log(std::to_string(packaged.assets.size()) + " asset(s) packaged");

    // 3. Save the authored scene as the game's initial scene.
    std::filesystem::create_directories(buildRoot / "Content" / "Scenes", ec);
    if (!m_editorScene->save_to_file((buildRoot / "Content" / "Scenes" / "Initial.scene").string())) {
        log("Build failed: could not save scene");
        return;
    }
    log("Scene saved to Content/Scenes/Initial.scene");

    // 4. Copy compiled shaders (the game falls back to Content/Shaders).
    const std::filesystem::path shaderSrc = std::filesystem::path(VULKANCRAFT_SHADER_DIR);
    size_t shaders = 0;
    if (std::filesystem::is_directory(shaderSrc)) {
        std::filesystem::create_directories(buildRoot / "Content" / "Shaders", ec);
        for (const auto& entry : std::filesystem::directory_iterator(shaderSrc, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".spv") continue;
            std::filesystem::copy_file(entry.path(), buildRoot / "Content" / "Shaders" / entry.path().filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) ++shaders;
        }
    }
    log(std::to_string(shaders) + " shader(s) copied");

    // 5. Copy the game executable next to the package.
    std::filesystem::create_directories(binDir, ec);
    const std::filesystem::path gameExe = sourceRoot / "build" / "Release" / "VulkanEngineGame.exe";
    if (std::filesystem::is_regular_file(gameExe)) {
        std::filesystem::copy_file(gameExe, binDir / "VulkanEngineGame.exe",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        log("Copied VulkanEngineGame.exe");
    } else {
        log("WARNING: " + gameExe.string() + " not found — build the VulkanEngineGame target first");
    }

    // 6. Package manifest + launcher script.
    std::ofstream manifest(buildRoot / "PackageManifest.txt", std::ios::trunc);
    manifest << "VulkanEngine.Package 1\nproject " << m_currentProjectName
             << "\ninitialScene Content/Scenes/Initial.scene\n";
    std::ofstream launcher(buildRoot / "run_game.bat", std::ios::trunc);
    launcher << "@echo off\ncd /d %~dp0\nBin\\VulkanEngineGame.exe\n";
    log("Build complete: " + buildRoot.string());
}

void EditorApplication::draw_console_panel() {
    ImGui::Begin(tr("Mensagens do Sistema", "Console & Profiler"));

    ImGui::TextColored(ImVec4(0.39f, 0.40f, 0.95f, 1.00f), "%s", tr("Vulkan Engine Studio 1.5.0 - Português (Brasil)", "Vulkan Engine Studio 1.5.0 - English (US)"));
    ImGui::Text(tr("Velocidade: %.1f FPS  |  Tempo por Quadro: %.2f ms  |  Memória RAM: %zu MB", "Speed: %.1f FPS  |  Frame Time: %.2f ms  |  RAM: %zu MB"), m_fps, m_frameTimeMs, m_ramUsageMb);
    ImGui::Separator();

    if (!m_buildLog.empty()) {
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("Registro do Build:", "Build Log:"));
        for (const std::string& line : m_buildLog) {
            ImGui::TextWrapped("%s", line.c_str());
        }
        ImGui::Separator();
    }

    ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("[INFO] Placa de Vídeo Vulkan 1.3 Inicializada: NVIDIA GeForce RTX 3060", "[INFO] Vulkan 1.3 Device Initialized: NVIDIA GeForce RTX 3060"));
    ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("[INFO] Mundo de Blocos Carregado. 1024 chunks ativos na memória.", "[INFO] VoxelWorldPlugin loaded. 1024 active chunks in memory."));
    ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.14f, 1.0f), "%s", tr("[AVISO] 12 imagens PNG não otimizadas encontradas na pasta Assets.", "[WARN] 12 Uncooked PNG assets found in Assets folder."));

    ImGui::End();
}

// ===========================================================================
// Vulkan helpers for the viewport
// ===========================================================================

namespace {

std::vector<uint32_t> read_spv(const char* name) {
    const std::string path = std::string(VULKANCRAFT_SHADER_DIR) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "[Editor] Cannot read shader: " << path << std::endl;
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0 || size % 4 != 0) {
        std::cerr << "[Editor] Invalid SPIR-V size for: " << name << std::endl;
        return {};
    }
    std::vector<uint32_t> spirv(static_cast<size_t>(size) / 4);
    in.read(reinterpret_cast<char*>(spirv.data()), size);
    return spirv;
}

VkShaderModule make_module(VkDevice device, const std::vector<uint32_t>& spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        std::cerr << "[Editor] Failed to create shader module" << std::endl;
        return VK_NULL_HANDLE;
    }
    return module;
}

VkPipeline create_scene_pipeline(VkDevice device, VkRenderPass renderPass, VkPipelineLayout layout,
                                 VkShaderModule vert, VkShaderModule frag,
                                 bool wireframe, bool depthTest, bool cull, bool withUv = false) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(EditorVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, pos)) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, normal)) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, color)) };
    attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, uv)) };
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = withUv ? 4u : 3u;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = wireframe ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlending;
    info.pDynamicState = &dynamicState;
    info.layout = layout;
    info.renderPass = renderPass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        std::cerr << "[Editor] Failed to create scene pipeline" << std::endl;
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// Unit cube (24 vertices with per-face normals, 36 indices). Vertex colors white.
void build_cube(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices) {
    const glm::vec3 n[6] = {
        { 0,  0, -1}, { 0,  0,  1}, {-1,  0,  0},
        { 1,  0,  0}, { 0, -1,  0}, { 0,  1,  0}
    };
    const glm::vec3 corners[8] = {
        {-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}, {-0.5f,  0.5f, -0.5f},
        {-0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f}
    };
    const uint32_t faces[6][4] = {
        {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
        {1, 5, 6, 2}, {4, 5, 1, 0}, {3, 2, 6, 7}
    };
    verts.clear();
    indices.clear();
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int c = 0; c < 4; ++c) {
            EditorVertex v;
            v.pos = corners[faces[f][c]];
            v.normal = n[f];
            v.color = glm::vec3(1.0f);
            verts.push_back(v);
        }
        indices.push_back(base);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

// Appends a cube (from build_cube) transformed by `model`; returns its index range.
std::pair<uint32_t, uint32_t> append_transformed_cube(std::vector<EditorVertex>& verts,
                                                      std::vector<uint32_t>& indices,
                                                      const glm::mat4& model, const glm::vec3& color) {
    std::vector<EditorVertex> cubeVerts;
    std::vector<uint32_t> cubeIndices;
    build_cube(cubeVerts, cubeIndices);
    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (const EditorVertex& v : cubeVerts) {
        EditorVertex out;
        out.pos = glm::vec3(model * glm::vec4(v.pos, 1.0f));
        out.normal = glm::normalize(glm::mat3(model) * v.normal);
        out.color = color;
        verts.push_back(out);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (uint32_t i : cubeIndices) indices.push_back(base + i);
    return { indexBase, static_cast<uint32_t>(cubeIndices.size()) };
}

// Cone along +Y from baseY to tipY with `segments` around the axis, then rotated by `rot`.
std::pair<uint32_t, uint32_t> append_cone(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                                          float baseY, float tipY, float radius, int segments,
                                          const glm::mat3& rot, const glm::vec3& color) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    const float step = glm::two_pi<float>() / static_cast<float>(segments);
    EditorVertex apex;
    apex.pos = glm::vec3(rot * glm::vec3(0.0f, tipY, 0.0f));
    apex.normal = glm::normalize(rot * glm::vec3(0.0f, 1.0f, 0.0f));
    apex.color = color;
    verts.push_back(apex);
    std::vector<uint32_t> ring;
    for (int s = 0; s < segments; ++s) {
        const float a = step * static_cast<float>(s);
        const glm::vec3 local(std::cos(a) * radius, baseY, std::sin(a) * radius);
        EditorVertex v;
        v.pos = glm::vec3(rot * local);
        const glm::vec3 toApex = glm::normalize(glm::vec3(0.0f, tipY, 0.0f) - local);
        const glm::vec3 tangent(std::sin(a), 0.0f, -std::cos(a));
        v.normal = glm::normalize(rot * glm::normalize(glm::cross(tangent, toApex)));
        v.color = color;
        verts.push_back(v);
        ring.push_back(static_cast<uint32_t>(verts.size()) - 1);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (int s = 0; s < segments; ++s) {
        const uint32_t next = ring[(s + 1) % segments];
        indices.push_back(base);
        indices.push_back(ring[s]);
        indices.push_back(next);
    }
    return { indexBase, static_cast<uint32_t>(segments) * 3u };
}

// Circle of `segments` in the plane perpendicular to `axis` (LINE_LIST).
std::pair<uint32_t, uint32_t> append_ring(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                                          const glm::vec3& axis, float radius, int segments,
                                          const glm::vec3& color) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    glm::vec3 u = glm::normalize(glm::cross(axis, glm::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    glm::vec3 v = glm::normalize(glm::cross(axis, u));
    for (int s = 0; s < segments; ++s) {
        const float a = glm::two_pi<float>() * static_cast<float>(s) / static_cast<float>(segments);
        EditorVertex vert;
        vert.pos = u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
        vert.normal = axis;
        vert.color = color;
        verts.push_back(vert);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (int s = 0; s < segments; ++s) {
        indices.push_back(base + static_cast<uint32_t>(s));
        indices.push_back(base + static_cast<uint32_t>((s + 1) % segments));
    }
    return { indexBase, static_cast<uint32_t>(segments) * 2u };
}

// Rotation matrix that maps +Y onto `axis` (used to place gizmo cones along an axis).
glm::mat3 rotation_axis_from_y(const glm::vec3& axis) {
    const glm::vec3 y(0, 1, 0);
    if (glm::length(glm::cross(y, axis)) < 1e-5f) {
        return axis.y > 0 ? glm::mat3(1.0f) : glm::mat3(glm::vec3(1, 0, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1));
    }
    const float angle = std::acos(glm::clamp(glm::dot(y, axis), -1.0f, 1.0f));
    return glm::mat3(glm::rotate(glm::mat4(1.0f), angle, glm::normalize(glm::cross(y, axis))));
}

glm::mat4 model_from_transform(const TransformComponent& t) {
    glm::mat4 model(1.0f);
    model = glm::translate(model, t.position);
    model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
    model = glm::scale(model, t.scale);
    return model;
}

void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& mvp, const glm::vec4& color) {
    const ScenePushConstants pc{ mvp, color };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(pc)), &pc);
}

} // namespace

// ─── Material-graph pipelines (README §16-18: graph → GLSL → Vulkan) ───
namespace {

// Compiles GLSL to SPIR-V via glslc (same tool as the ShaderCompiler target).
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

// std140 sizes/alignments for the material params UBO (matching the generated
// GLSL layout): vec3 has 16-byte base alignment, so float/vec2 after it must be
// written at padded offsets.
size_t material_std140_size(Rendering::MaterialValueType type) {
    switch (type) {
        case Rendering::MaterialValueType::Bool: return 4;
        case Rendering::MaterialValueType::Float: return 4;
        case Rendering::MaterialValueType::Vec2: return 8;
        case Rendering::MaterialValueType::Vec3: return 12;
        case Rendering::MaterialValueType::Vec4: return 16;
        case Rendering::MaterialValueType::Texture2D: return 16;
    }
    return 4;
}

size_t material_std140_alignment(Rendering::MaterialValueType type) {
    switch (type) {
        case Rendering::MaterialValueType::Bool: return 4;
        case Rendering::MaterialValueType::Float: return 4;
        case Rendering::MaterialValueType::Vec2: return 8;
        case Rendering::MaterialValueType::Vec3: return 16;
        case Rendering::MaterialValueType::Vec4: return 16;
        case Rendering::MaterialValueType::Texture2D: return 16;
    }
    return 4;
}

size_t align_material_offset(size_t offset, size_t alignment) {
    return (offset + alignment - 1) / alignment * alignment;
}

void write_ubo_value(std::byte* dst, Rendering::MaterialValueType type, const Rendering::MaterialValue& value) {
    std::memset(dst, 0, material_std140_size(type));
    switch (type) {
        case Rendering::MaterialValueType::Bool:
            *reinterpret_cast<uint32_t*>(dst) = std::holds_alternative<bool>(value) && std::get<bool>(value) ? 1u : 0u;
            break;
        case Rendering::MaterialValueType::Float:
            if (std::holds_alternative<float>(value)) *reinterpret_cast<float*>(dst) = std::get<float>(value);
            break;
        case Rendering::MaterialValueType::Vec2:
            if (std::holds_alternative<glm::vec2>(value)) {
                const glm::vec2 v = std::get<glm::vec2>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Vec3:
            if (std::holds_alternative<glm::vec3>(value)) {
                const glm::vec3 v = std::get<glm::vec3>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Vec4:
            if (std::holds_alternative<glm::vec4>(value)) {
                const glm::vec4 v = std::get<glm::vec4>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Texture2D:
            break;
    }
}

// Content hash of a material graph → rebuild pipelines when the graph changes.
uint64_t hash_material_graph(const Rendering::MaterialGraph& graph) {
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const auto& node : graph.nodes()) {
        mix(&node.id, sizeof(node.id));
        const auto kind = static_cast<uint8_t>(node.kind);
        mix(&kind, 1);
        const auto outputType = static_cast<uint8_t>(node.outputType);
        mix(&outputType, 1);
        mix(node.label.data(), node.label.size());
        mix(node.parameter.data(), node.parameter.size());
        std::visit([&](const auto& v) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                mix(v.data(), v.size());
            } else {
                mix(&v, sizeof(v));
            }
        }, node.value);
    }
    for (const auto& p : graph.parameters()) {
        mix(p.name.data(), p.name.size());
        const auto type = static_cast<uint8_t>(p.type);
        mix(&type, 1);
        mix(&p.exposed, 1);
    }
    return h;
}

// Default graph for a cooked MaterialAsset: PBR params exposed as UBO members.
Rendering::MaterialGraph material_graph_from_asset(const MaterialAsset& mat) {
    Rendering::MaterialGraph graph;
    graph.define_parameter({ "Albedo", Rendering::MaterialValueType::Vec3, mat.albedo, true });
    graph.define_parameter({ "Roughness", Rendering::MaterialValueType::Float, mat.roughness, true });
    graph.define_parameter({ "Metallic", Rendering::MaterialValueType::Float, mat.metallic, true });
    graph.define_parameter({ "Emissive", Rendering::MaterialValueType::Vec3,
                             mat.emissiveColor * mat.emissiveIntensity, true });
    const auto albedo = graph.add_parameter("Albedo");
    const auto roughness = graph.add_parameter("Roughness");
    const auto metallic = graph.add_parameter("Metallic");
    const auto emissive = graph.add_parameter("Emissive");
    const auto baseOut = graph.add_output("BaseColor", Rendering::MaterialValueType::Vec3);
    const auto roughOut = graph.add_output("Roughness", Rendering::MaterialValueType::Float);
    const auto metalOut = graph.add_output("Metallic", Rendering::MaterialValueType::Float);
    const auto emisOut = graph.add_output("Emissive", Rendering::MaterialValueType::Vec3);
    (void)graph.connect(albedo, baseOut, 0);
    (void)graph.connect(roughness, roughOut, 0);
    (void)graph.connect(metallic, metalOut, 0);
    (void)graph.connect(emissive, emisOut, 0);
    return graph;
}

} // namespace

uint32_t EditorApplication::find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

void EditorApplication::create_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                     VkMemoryPropertyFlags memProps, VkImage& image, VkDeviceMemory& memory,
                                     uint32_t mipLevels /* = 1 */) {
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = { w, h, 1 };
    info.mipLevels = std::max(mipLevels, 1u);
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_device, &info, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image");
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(m_device, image, &requirements);
    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, memProps);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate image memory");
    }
    vkBindImageMemory(m_device, image, memory, 0);
}

VkImageView EditorApplication::create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                                 uint32_t mipLevels /* = 1 */) {
    VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = { aspect, 0, std::max(mipLevels, 1u), 0, 1 };
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(m_device, &info, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image view");
    }
    return view;
}

void EditorApplication::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                      VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &info, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, props);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory");
    }
    vkBindBufferMemory(m_device, buffer, memory, 0);
}

void EditorApplication::transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                                VkImageAspectFlags aspect, uint32_t baseMipLevel /* = 0 */,
                                                uint32_t levelCount /* = 1 */) {
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, baseMipLevel, std::max(levelCount, 1u), 0, 1 };
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkCommandBuffer EditorApplication::begin_single_time_commands() {
    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void EditorApplication::end_single_time_commands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
}

// ===========================================================================
// Viewport initialization
// ===========================================================================

void EditorApplication::init_offscreen_target() {
    // Render passes and sampler are size-independent and referenced by the
    // pipelines, so they are created once and kept until final cleanup.
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = colorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkAttachmentDescription attachments[2] = { colorAttachment, depthAttachment };
    VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_offscreen.renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen render pass");
    }

    // Pick render pass: color only writes, shared depth attachment.
    VkAttachmentDescription pickColor{};
    pickColor.format = colorFormat;
    pickColor.samples = VK_SAMPLE_COUNT_1_BIT;
    pickColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pickColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    pickColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    pickColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    pickColor.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pickColor.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentDescription pickAttachments[2] = { pickColor, depthAttachment };
    VkRenderPassCreateInfo pickRpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    pickRpInfo.attachmentCount = 2;
    pickRpInfo.pAttachments = pickAttachments;
    pickRpInfo.subpassCount = 1;
    pickRpInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(m_device, &pickRpInfo, nullptr, &m_offscreen.pickRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pick render pass");
    }

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_offscreen.sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen sampler");
    }

    create_offscreen_buffers(800, 600);
    create_shadow_map();
}

// Creates the size-dependent resources (images, views, framebuffers, staging).
// Render passes and the sampler are kept across resizes.
void EditorApplication::create_offscreen_buffers(uint32_t w, uint32_t h) {
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    create_image(w, h, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.colorImage, m_offscreen.colorMemory);
    m_offscreen.colorView = create_image_view(m_offscreen.colorImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);

    create_image(w, h, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.depthImage, m_offscreen.depthMemory);
    m_offscreen.depthView = create_image_view(m_offscreen.depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    create_image(w, h, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.pickImage, m_offscreen.pickMemory);
    m_offscreen.pickView = create_image_view(m_offscreen.pickImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);

    const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(w) * h * 4;
    create_buffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_offscreen.pickStagingBuffer, m_offscreen.pickStagingMemory);

    // Bring freshly created images into the layouts their render passes expect.
    {
        VkCommandBuffer transitionCmd = begin_single_time_commands();
        transition_image_layout(transitionCmd, m_offscreen.colorImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition_image_layout(transitionCmd, m_offscreen.depthImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
        transition_image_layout(transitionCmd, m_offscreen.pickImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        end_single_time_commands(transitionCmd);
    }

    VkImageView attachments[2] = { m_offscreen.colorView, m_offscreen.depthView };
    VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = m_offscreen.renderPass;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = attachments;
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_offscreen.framebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen framebuffer");
    }

    VkImageView pickAttachments[2] = { m_offscreen.pickView, m_offscreen.depthView };
    VkFramebufferCreateInfo pickFbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    pickFbInfo.renderPass = m_offscreen.pickRenderPass;
    pickFbInfo.attachmentCount = 2;
    pickFbInfo.pAttachments = pickAttachments;
    pickFbInfo.width = w;
    pickFbInfo.height = h;
    pickFbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &pickFbInfo, nullptr, &m_offscreen.pickFramebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pick framebuffer");
    }

    m_offscreen.width = w;
    m_offscreen.height = h;
    m_offscreen.imguiTextureID = ImGui_ImplVulkan_AddTexture(
        m_offscreen.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Sun shadow map: fixed-size depth-only target, rebuilt once at startup. The
// viewport records a shadow pass before the scene pass and the material
// pipelines sample this map through the comparison sampler.
void EditorApplication::create_shadow_map() {
    destroy_shadow_map();
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    create_image(m_shadowMap.size, m_shadowMap.size, depthFormat,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_shadowMap.image, m_shadowMap.memory);
    m_shadowMap.view = create_image_view(m_shadowMap.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Depth-only render pass: the map ends in SHADER_READ_ONLY so the material
    // shaders can sample it without an extra transition.
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthRef{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &depthAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_shadowMap.renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow render pass");
    }
    VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = m_shadowMap.renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_shadowMap.view;
    fbInfo.width = m_shadowMap.size;
    fbInfo.height = m_shadowMap.size;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_shadowMap.framebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow framebuffer");
    }

    // Comparison sampler: depth values are fetched raw (compareEnable is
    // inert for non-shadow sampler types) and the shader does the PCF-style
    // bias compare — same arrangement as the game's shadow path.
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowMap.sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow sampler");
    }

    // Position-only shaders (vertex reads all EditorVertex attributes to keep
    // the same vertex input state; only pos is used).
    const std::string vertSrc =
        "#version 450\n"
        "layout(push_constant) uniform Push { mat4 mvp; } pc;\n"
        "layout(location = 0) in vec3 inPos;\n"
        "layout(location = 1) in vec3 inNormal;\n"
        "layout(location = 2) in vec3 inColor;\n"
        "layout(location = 3) in vec2 inUv;\n"
        "void main() { gl_Position = pc.mvp * vec4(inPos, 1.0); }\n";
    const std::string fragSrc = "#version 450\nvoid main() {}\n";
    const std::vector<uint32_t> vertSpv = compile_material_glsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    const std::vector<uint32_t> fragSpv = compile_material_glsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vertSpv.empty() || fragSpv.empty()) {
        throw std::runtime_error("Failed to compile shadow shaders");
    }
    m_shadowMap.vertShader = make_module(m_device, vertSpv);
    m_shadowMap.fragShader = make_module(m_device, fragSpv);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_shadowMap.pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline layout");
    }

    // Depth-only graphics pipeline (no color attachment, depth write on).
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_shadowMap.vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_shadowMap.fragShader;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(EditorVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, pos)) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, normal)) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, color)) };
    attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, uv)) };
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 4;
    vertexInput.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendStateCreateInfo colorBlending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.attachmentCount = 0;
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlending;
    info.pDynamicState = &dynamicState;
    info.layout = m_shadowMap.pipelineLayout;
    info.renderPass = m_shadowMap.renderPass;
    info.subpass = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_shadowMap.pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline");
    }
}

void EditorApplication::destroy_shadow_map() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_shadowMap.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_shadowMap.pipeline, nullptr);
    if (m_shadowMap.pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_shadowMap.pipelineLayout, nullptr);
    if (m_shadowMap.vertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_shadowMap.vertShader, nullptr);
    if (m_shadowMap.fragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_shadowMap.fragShader, nullptr);
    if (m_shadowMap.sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_shadowMap.sampler, nullptr);
    if (m_shadowMap.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_shadowMap.framebuffer, nullptr);
    if (m_shadowMap.renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_shadowMap.renderPass, nullptr);
    if (m_shadowMap.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_shadowMap.view, nullptr);
    if (m_shadowMap.image != VK_NULL_HANDLE) vkDestroyImage(m_device, m_shadowMap.image, nullptr);
    if (m_shadowMap.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_shadowMap.memory, nullptr);
    m_shadowMap = EditorShadowMap{};
}

void EditorApplication::recreate_offscreen_if_needed(uint32_t w, uint32_t h) {
    if (m_offscreen.framebuffer != VK_NULL_HANDLE && m_offscreen.width == w && m_offscreen.height == h) {
        return;
    }
    if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device); // old attachments are in flight
    cleanup_offscreen_target();
    create_offscreen_buffers(w, h);
}

void EditorApplication::cleanup_offscreen_target() {
    if (m_device == VK_NULL_HANDLE) return;
    destroy_shadow_map();
    if (m_offscreen.imguiTextureID != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_offscreen.imguiTextureID);
        m_offscreen.imguiTextureID = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickStagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_offscreen.pickStagingBuffer, nullptr);
        m_offscreen.pickStagingBuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickStagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.pickStagingMemory, nullptr);
        m_offscreen.pickStagingMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_offscreen.pickFramebuffer, nullptr);
        m_offscreen.pickFramebuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.pickView, nullptr);
        m_offscreen.pickView = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.pickImage, nullptr);
        m_offscreen.pickImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.pickMemory, nullptr);
        m_offscreen.pickMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_offscreen.framebuffer, nullptr);
        m_offscreen.framebuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.depthView, nullptr);
        m_offscreen.depthView = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.depthImage, nullptr);
        m_offscreen.depthImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.depthMemory, nullptr);
        m_offscreen.depthMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.colorView, nullptr);
        m_offscreen.colorView = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.colorImage, nullptr);
        m_offscreen.colorImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.colorMemory, nullptr);
        m_offscreen.colorMemory = VK_NULL_HANDLE;
    }
    m_offscreen.width = 0;
    m_offscreen.height = 0;
}

void EditorApplication::init_scene_pipeline() {
    m_viewportVertShader = make_module(m_device, read_spv("editor_viewport.vert.spv"));
    m_viewportFragShader = make_module(m_device, read_spv("editor_viewport.frag.spv"));
    m_pickFragShader = make_module(m_device, read_spv("editor_pick.frag.spv"));
    if (!m_viewportVertShader || !m_viewportFragShader || !m_pickFragShader) {
        throw std::runtime_error("Editor viewport shaders failed to compile (run the compile_shaders target)");
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ScenePushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_scenePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create scene pipeline layout");
    }

    m_scenePipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                            m_viewportVertShader, m_viewportFragShader,
                                            false, true, true);
    m_wireframePipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                                m_viewportVertShader, m_viewportFragShader,
                                                true, false, false);
    m_gizmoPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                            m_viewportVertShader, m_viewportFragShader,
                                            false, false, false);
    m_pickPipeline = create_scene_pipeline(m_device, m_offscreen.pickRenderPass, m_scenePipelineLayout,
                                           m_viewportVertShader, m_pickFragShader,
                                           false, true, true);
    if (!m_scenePipeline || !m_wireframePipeline || !m_gizmoPipeline || !m_pickPipeline) {
        throw std::runtime_error("Failed to create viewport pipelines");
    }
}

void EditorApplication::init_geometry_buffers() {
    // Cube
    std::vector<EditorVertex> cubeVerts;
    std::vector<uint32_t> cubeIndices;
    generate_cube_geometry(cubeVerts, cubeIndices);
    m_cubeIndexCount = static_cast<uint32_t>(cubeIndices.size());
    VkDeviceSize cubeVBsize = sizeof(EditorVertex) * cubeVerts.size();
    VkDeviceSize cubeIBsize = sizeof(uint32_t) * cubeIndices.size();
    create_buffer(cubeVBsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cubeVB.buffer, m_cubeVB.memory);
    create_buffer(cubeIBsize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cubeIB.buffer, m_cubeIB.memory);
    void* data = nullptr;
    vkMapMemory(m_device, m_cubeVB.memory, 0, cubeVBsize, 0, &data);
    std::memcpy(data, cubeVerts.data(), static_cast<size_t>(cubeVBsize));
    vkUnmapMemory(m_device, m_cubeVB.memory);
    vkMapMemory(m_device, m_cubeIB.memory, 0, cubeIBsize, 0, &data);
    std::memcpy(data, cubeIndices.data(), static_cast<size_t>(cubeIBsize));
    vkUnmapMemory(m_device, m_cubeIB.memory);

    // Grid (XZ plane)
    std::vector<EditorVertex> gridVerts;
    generate_grid_geometry(gridVerts, 60.0f, 1.0f);
    m_gridVertexCount = static_cast<uint32_t>(gridVerts.size());
    VkDeviceSize gridSize = sizeof(EditorVertex) * gridVerts.size();
    create_buffer(gridSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_gridVB.buffer, m_gridVB.memory);
    vkMapMemory(m_device, m_gridVB.memory, 0, gridSize, 0, &data);
    std::memcpy(data, gridVerts.data(), static_cast<size_t>(gridSize));
    vkUnmapMemory(m_device, m_gridVB.memory);

    // Light icon (octahedron edges)
    std::vector<EditorVertex> lightVerts;
    generate_light_icon(lightVerts);
    m_lightIconVertexCount = static_cast<uint32_t>(lightVerts.size());
    VkDeviceSize lightSize = sizeof(EditorVertex) * lightVerts.size();
    create_buffer(lightSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_lightIconVB.buffer, m_lightIconVB.memory);
    vkMapMemory(m_device, m_lightIconVB.memory, 0, lightSize, 0, &data);
    std::memcpy(data, lightVerts.data(), static_cast<size_t>(lightSize));
    vkUnmapMemory(m_device, m_lightIconVB.memory);

    // Camera icon (pyramid edges)
    std::vector<EditorVertex> cameraVerts;
    generate_camera_icon(cameraVerts);
    m_cameraIconVertexCount = static_cast<uint32_t>(cameraVerts.size());
    VkDeviceSize cameraSize = sizeof(EditorVertex) * cameraVerts.size();
    create_buffer(cameraSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cameraIconVB.buffer, m_cameraIconVB.memory);
    vkMapMemory(m_device, m_cameraIconVB.memory, 0, cameraSize, 0, &data);
    std::memcpy(data, cameraVerts.data(), static_cast<size_t>(cameraSize));
    vkUnmapMemory(m_device, m_cameraIconVB.memory);

    // Gizmo geometry (all modes)
    generate_gizmo_geometry();
}

void EditorApplication::generate_cube_geometry(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices) {
    build_cube(verts, indices);
}

void EditorApplication::generate_grid_geometry(std::vector<EditorVertex>& verts, float extent, float step) {
    verts.clear();
    const int halfLines = static_cast<int>(extent / step);
    for (int i = -halfLines; i <= halfLines; ++i) {
        const float pos = static_cast<float>(i) * step;
        const glm::vec3 color = (i % 5 == 0) ? glm::vec3(0.42f, 0.46f, 0.58f) : glm::vec3(0.28f, 0.30f, 0.40f);
        EditorVertex a, b;
        a.pos = { -extent, 0.0f, pos }; a.normal = { 0, 1, 0 }; a.color = color;
        b.pos = {  extent, 0.0f, pos }; b.normal = { 0, 1, 0 }; b.color = color;
        verts.push_back(a); verts.push_back(b);
        a.pos = { pos, 0.0f, -extent }; b.pos = { pos, 0.0f, extent };
        verts.push_back(a); verts.push_back(b);
    }
}

void EditorApplication::generate_light_icon(std::vector<EditorVertex>& verts) {
    verts.clear();
    const glm::vec3 color(1.0f);
    const float r = 0.45f;
    const glm::vec3 pts[6] = {
        { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, r }, { 0, 0, -r }
    };
    const int edges[12][2] = {
        {0, 2}, {0, 3}, {0, 4}, {0, 5},
        {1, 2}, {1, 3}, {1, 4}, {1, 5},
        {2, 4}, {4, 3}, {3, 5}, {5, 2}
    };
    for (const auto& e : edges) {
        EditorVertex a, b;
        a.pos = pts[e[0]]; a.normal = { 0, 1, 0 }; a.color = color;
        b.pos = pts[e[1]]; b.normal = { 0, 1, 0 }; b.color = color;
        verts.push_back(a); verts.push_back(b);
    }
}

void EditorApplication::generate_camera_icon(std::vector<EditorVertex>& verts) {
    verts.clear();
    const glm::vec3 color(1.0f);
    // Pyramid pointing +Z: apex behind, near rectangle in front.
    const glm::vec3 apex(0.0f, 0.0f, -0.55f);
    const glm::vec3 corners[4] = {
        { -0.42f, -0.30f, 0.45f }, { 0.42f, -0.30f, 0.45f },
        { 0.42f, 0.30f, 0.45f }, { -0.42f, 0.30f, 0.45f }
    };
    const int edges[8][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {0, 4}, {1, 4}, {2, 4}, {3, 4}
    };
    glm::vec3 pts[5];
    pts[0] = corners[0]; pts[1] = corners[1]; pts[2] = corners[2]; pts[3] = corners[3]; pts[4] = apex;
    for (const auto& e : edges) {
        EditorVertex a, b;
        a.pos = pts[e[0]]; a.normal = { 0, 0, 1 }; a.color = color;
        b.pos = pts[e[1]]; b.normal = { 0, 0, 1 }; b.color = color;
        verts.push_back(a); verts.push_back(b);
    }
}

void EditorApplication::generate_gizmo_geometry() {
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    const float shaftLen = 1.55f;
    const float ringRadius = 1.45f;

    // Shafts (LINE_LIST) + cones (translate) + rings (rotate) + cubes (scale).
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 dir = kAxisDirs[axis];
        const glm::vec3 color = kAxisColors[axis];

        // Shaft from origin to 82% of the length.
        const uint32_t shaftBase = static_cast<uint32_t>(verts.size());
        EditorVertex origin, tip;
        origin.pos = glm::vec3(0.0f); origin.normal = dir; origin.color = color;
        tip.pos = dir * (shaftLen * 0.82f); tip.normal = dir; tip.color = color;
        verts.push_back(origin);
        verts.push_back(tip);
        m_gizmoShaftRanges[axis] = { static_cast<uint32_t>(indices.size()), 2 };
        indices.push_back(shaftBase);
        indices.push_back(shaftBase + 1);

        // Translate arrow cone.
        {
            const auto [offset, count] = append_cone(verts, indices, shaftLen * 0.72f, shaftLen, 0.09f, 12,
                                                     rotation_axis_from_y(dir), color);
            m_gizmoArrowRanges[axis] = GizmoDrawRange{ offset, count };
        }

        // Rotate ring (perpendicular to the axis).
        {
            const auto [offset, count] = append_ring(verts, indices, dir, ringRadius, 48, color);
            m_gizmoRingRanges[axis] = GizmoDrawRange{ offset, count };
        }

        // Scale tip cube at the end of the shaft.
        {
            glm::mat4 tipModel = glm::translate(glm::mat4(1.0f), dir * shaftLen);
            tipModel = glm::scale(tipModel, glm::vec3(0.17f));
            const auto [offset, count] = append_transformed_cube(verts, indices, tipModel, color);
            m_gizmoTipRanges[axis] = GizmoDrawRange{ offset, count };
        }
    }

    VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_gizmoVB.buffer, m_gizmoVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_gizmoIB.buffer, m_gizmoIB.memory);
    void* data = nullptr;
    vkMapMemory(m_device, m_gizmoVB.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, m_gizmoVB.memory);
    vkMapMemory(m_device, m_gizmoIB.memory, 0, ibSize, 0, &data);
    std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
    vkUnmapMemory(m_device, m_gizmoIB.memory);
}

// ===========================================================================
// Viewport rendering
// ===========================================================================

namespace {

void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}

void draw_line_list(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, uint32_t vertexCount,
                    const glm::mat4& mvp, const glm::vec4& color) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    push_constants(cmd, layout, mvp, color);
    vkCmdDraw(cmd, vertexCount, 1, 0, 0);
}

} // namespace

void EditorApplication::record_shadow_pass(VkCommandBuffer cmd, const Scene* scene) {
    m_shadowMap.enabled = false;
    if (m_shadowMap.pipeline == VK_NULL_HANDLE) return;

    // Sun direction from the scene's directional sun (or a fixed default).
    glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
    bool hasSun = false;
    if (scene) {
        for (const auto& [id, light] : scene->lightComponents) {
            if (!is_directional_sun(light)) continue;
            const auto tit = scene->transformComponents.find(id);
            if (tit != scene->transformComponents.end()) {
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                sunDir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            hasSun = true;
            break;
        }
    }
    if (!hasSun) return;

    // Ortho fit around the camera, depth remapped to [0,1] so the shared
    // computeShadow (sc.z in [0,1] after the divide) matches the stored depth.
    const glm::vec3 lightDir = glm::normalize(-sunDir);
    const glm::vec3 center = m_editorCamera.position;
    constexpr float kExtent = 35.0f;
    const glm::mat4 lightView = glm::lookAt(center + lightDir * 80.0f, center, glm::vec3(0, 1, 0));
    const glm::mat4 lightProj = glm::ortho(-kExtent, kExtent, -kExtent, kExtent, 0.1f, 200.0f);
    glm::mat4 depthRemap(1.0f);
    depthRemap[2][2] = 0.5f;
    depthRemap[2][3] = 0.5f;
    m_shadowMap.viewProj = depthRemap * lightProj * lightView;
    m_shadowMap.enabled = true;

    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = m_shadowMap.renderPass;
    info.framebuffer = m_shadowMap.framebuffer;
    info.renderArea.offset = { 0, 0 };
    info.renderArea.extent = { m_shadowMap.size, m_shadowMap.size };
    VkClearValue clear;
    clear.depthStencil = { 1.0f, 0 };
    info.clearValueCount = 1;
    info.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport_scissor(cmd, m_shadowMap.size, m_shadowMap.size);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowMap.pipeline);

    // Casters: every entity with a mesh renderer and a loaded .vcmesh.
    for (const auto& [id, ent] : scene->get_entities()) {
        const auto transformIt = scene->transformComponents.find(id);
        if (transformIt == scene->transformComponents.end()) continue;
        const auto meshComp = scene->meshRendererComponents.find(id);
        if (meshComp == scene->meshRendererComponents.end() ||
            !meshComp->second.meshAssetID.is_valid()) continue;
        const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID);
        if (!mesh) continue;
        const glm::mat4 mvp = m_shadowMap.viewProj * model_from_transform(transformIt->second);
        vkCmdPushConstants(cmd, m_shadowMap.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &mvp);
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vb.buffer, &vertexOffset);
        if (mesh->ib.buffer != VK_NULL_HANDLE)
            vkCmdBindIndexBuffer(cmd, mesh->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const auto& range : mesh->ranges) {
            if (range.indexed)
                vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
            else
                vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
        }
    }
    vkCmdEndRenderPass(cmd);
}

void EditorApplication::render_scene_to_offscreen(VkCommandBuffer cmd) {
    Scene* renderScene = m_playMode.get_active_scene();
    if (!renderScene) renderScene = m_editorScene.get();
    record_shadow_pass(cmd, renderScene);

    build_viewport_render_graph();
    if (!m_viewportRenderGraphExecutor.valid() || m_offscreen.framebuffer == VK_NULL_HANDLE) return;

    // Re-register the scene pass every frame so the framebuffer stays current
    // after offscreen recreations (resize) — the executor keeps its own copy.
    Rendering::VulkanRenderGraphExecutor::PassFrame sceneFrame;
    sceneFrame.renderPass = m_offscreen.renderPass;
    sceneFrame.framebuffers = { m_offscreen.framebuffer };
    sceneFrame.clearValues.resize(2);
    sceneFrame.clearValues[0].color = { { 0.11f, 0.13f, 0.18f, 1.0f } };
    sceneFrame.clearValues[1].depthStencil = { 1.0f, 0 };
    sceneFrame.draw = [this](VkCommandBuffer cb) { record_viewport_scene_content(cb); };
    m_viewportRenderGraphExecutor.register_pass(m_viewportScenePass, std::move(sceneFrame));

    // Scene pass recorded by the compiled graph: begins the offscreen render
    // pass, runs the content callback, ends — same executor the game uses.
    m_viewportRenderGraphExecutor.record(cmd, 0, { m_offscreen.width, m_offscreen.height });
}

void EditorApplication::build_viewport_render_graph() {
    if (m_viewportRenderGraphBuilt) return;
    using namespace Engine::Rendering;
    const auto colorRes = m_viewportRenderGraph.add_resource({ "Viewport Color", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    const auto depthRes = m_viewportRenderGraph.add_resource({ "Viewport Depth", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    m_viewportScenePass = m_viewportRenderGraph.add_pass({ "Scene", RenderQueue::Graphics,
        { { colorRes, RenderAccess::Write, RenderResourceState::ColorAttachment },
          { depthRes, RenderAccess::Write, RenderResourceState::DepthAttachment } }, true });
    std::string error;
    if (!m_viewportRenderGraphExecutor.initialize(m_device, m_viewportRenderGraph, &error)) {
        std::cerr << "[Editor] viewport render graph init failed: " << error << std::endl;
        return;
    }
    m_viewportRenderGraphBuilt = true;
    std::cout << "[Editor] Viewport render graph wired ("
              << m_viewportRenderGraphExecutor.compile_result().order.size() << " passes, "
              << m_viewportRenderGraphExecutor.compile_result().barriers.size() << " barriers)\n";
}

void EditorApplication::record_viewport_scene_content(VkCommandBuffer cmd) {
    Scene* renderScene = m_playMode.get_active_scene();
    if (!renderScene) renderScene = m_editorScene.get();

    set_viewport_scissor(cmd, m_offscreen.width, m_offscreen.height);

    const float aspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();

    // Grid (wireframe overlay).
    if (m_gridVB.buffer != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
        draw_line_list(cmd, m_scenePipelineLayout, m_gridVB.buffer, m_gridVertexCount, viewProj, glm::vec4(1.0f));
    }

    // Scene entities (renderScene resolved at the top of this function).
    if (renderScene) {
        for (const auto& [id, ent] : renderScene->get_entities()) {
            const auto transformIt = renderScene->transformComponents.find(id);
            if (transformIt == renderScene->transformComponents.end()) continue;
            const TransformComponent& t = transformIt->second;
            const bool selected = m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id;

            if (renderScene->lightComponents.contains(id)) {
                draw_light_icon(cmd, viewProj, t, selected);
            } else if (renderScene->cameraComponents.contains(id)) {
                draw_camera_frustum(cmd, viewProj, t, selected);
            } else if (renderScene->meshRendererComponents.contains(id) ||
                       renderScene->materialComponents.contains(id)) {
                glm::vec3 baseColor(0.72f, 0.75f, 0.82f);
                if (renderScene->materialComponents.contains(id)) {
                    baseColor = renderScene->materialComponents.at(id).albedo;
                }
                const glm::vec4 color = selected
                    ? glm::vec4(0.45f, 0.50f, 1.00f, 1.0f)
                    : glm::vec4(baseColor, 1.0f);
                bool drewMesh = false;
                const auto meshComp = renderScene->meshRendererComponents.find(id);
                if (meshComp != m_editorScene->meshRendererComponents.end() &&
                    meshComp->second.meshAssetID.is_valid()) {
                    if (const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID)) {
                        // Material-graph path: the mesh renderer's material asset,
                        // or the Material Editor's live graph on the selected entity.
                        GraphMaterialPipeline* gmp = nullptr;
                        const bool useLive = m_specializedEditors.previewOnSelected && selected;
                        if (useLive) {
                            const uint64_t liveHash = hash_material_graph(m_specializedEditors.live_material_graph());
                            if (liveHash != m_liveGraphHash || !m_liveGraphPipeline.valid) {
                                destroy_graph_pipeline(m_liveGraphPipeline);
                                if (!build_graph_pipeline(m_specializedEditors.live_material_graph(), m_liveGraphPipeline)) {
                                    if (!m_liveGraphLastErrorLogged) {
                                        std::cerr << "[Editor] Material preview: " << m_liveGraphPipeline.lastError << std::endl;
                                        m_liveGraphLastErrorLogged = true;
                                    }
                                } else {
                                    m_liveGraphLastErrorLogged = false;
                                }
                                m_liveGraphHash = liveHash;
                            }
                            gmp = m_liveGraphPipeline.valid ? &m_liveGraphPipeline : nullptr;
                        } else if (meshComp->second.materialAssetID.is_valid() &&
                                   load_material_asset(meshComp->second.materialAssetID)) {
                            const UUID matId = meshComp->second.materialAssetID;
                            const MaterialAsset& mat = m_materialAssets.at(matId);
                            const Rendering::MaterialGraph graph = material_graph_from_asset(mat);
                            const uint64_t graphHash = hash_material_graph(graph);
                            auto it = m_graphMaterialPipelines.find(matId);
                            if (it == m_graphMaterialPipelines.end() ||
                                !it->second.valid || it->second.graphHash != graphHash) {
                                if (it != m_graphMaterialPipelines.end()) destroy_graph_pipeline(it->second);
                                GraphMaterialPipeline built;
                                built.graphHash = graphHash;
                                if (!build_graph_pipeline(graph, built)) {
                                    std::cerr << "[Editor] Material pipeline: " << built.lastError << std::endl;
                                }
                                it = m_graphMaterialPipelines.insert_or_assign(matId, std::move(built)).first;
                            }
                            if (it->second.valid) gmp = &it->second;
                        }
                        if (gmp) {
                            const MaterialAsset* matAsset = nullptr;
                            const auto matAssetIt = m_materialAssets.find(meshComp->second.materialAssetID);
                            if (matAssetIt != m_materialAssets.end()) matAsset = &matAssetIt->second;
                            const MaterialComponent* comp = nullptr;
                            const auto compIt = renderScene->materialComponents.find(id);
                            if (compIt != m_editorScene->materialComponents.end()) comp = &compIt->second;
                            write_material_ubo(*gmp, matAsset, comp);
                            write_light_ubo(*gmp, renderScene, m_editorCamera.position);
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->pipeline);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->layout,
                                                    0, 1, &gmp->descriptorSet, 0, nullptr);
                            const glm::mat4 model = model_from_transform(t);
                            const Rendering::MaterialPushConstants pc{ viewProj * model, model };
                            vkCmdPushConstants(cmd, gmp->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                               sizeof(pc), &pc);
                            const VkDeviceSize vertexOffset = 0;
                            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vb.buffer, &vertexOffset);
                            if (mesh->ib.buffer != VK_NULL_HANDLE)
                                vkCmdBindIndexBuffer(cmd, mesh->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                            for (const auto& range : mesh->ranges) {
                                if (range.indexed)
                                    vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
                                else
                                    vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
                            }
                            drewMesh = true;
                        } else {
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                            draw_mesh_resource(cmd, viewProj * model_from_transform(t), color, *mesh);
                            drewMesh = true;
                        }
                    }
                }
                if (!drewMesh) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                      m_cubeIndexCount, viewProj * model_from_transform(t), color);
                }
                if (selected) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
                    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                      m_cubeIndexCount, viewProj * model_from_transform(t),
                                      glm::vec4(0.55f, 0.60f, 1.00f, 1.0f));
                }
            } else {
                // Transform-only entity: subtle wireframe box.
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
                draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                  m_cubeIndexCount, viewProj * model_from_transform(t),
                                  selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f)
                                           : glm::vec4(0.35f, 0.38f, 0.50f, 1.0f));
            }

            if (renderScene->rigidbodyComponents.contains(id)) {
                draw_collider_wireframe(cmd, viewProj, t, selected);
            }
        }
    }

    // Gizmo on the selected entity (drawn every frame; the active axis is
    // highlighted while dragging).
    draw_gizmo_overlay(cmd, viewProj);
}

void EditorApplication::draw_light_icon(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                        const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), t.position) * glm::scale(glm::mat4(1.0f), glm::vec3(1.2f));
    const glm::vec4 color = selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f) : glm::vec4(1.0f, 0.85f, 0.35f, 1.0f);
    draw_line_list(cmd, m_scenePipelineLayout, m_lightIconVB.buffer, m_lightIconVertexCount,
                   viewProj * model, color);
}

void EditorApplication::draw_camera_frustum(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                            const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), t.position)
                          * glm::rotate(glm::mat4(1.0f), glm::radians(t.rotation.y), glm::vec3(0, 1, 0))
                          * glm::rotate(glm::mat4(1.0f), glm::radians(t.rotation.x), glm::vec3(1, 0, 0))
                          * glm::scale(glm::mat4(1.0f), glm::vec3(1.4f));
    const glm::vec4 color = selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f) : glm::vec4(0.35f, 0.75f, 1.00f, 1.0f);
    draw_line_list(cmd, m_scenePipelineLayout, m_cameraIconVB.buffer, m_cameraIconVertexCount,
                   viewProj * model, color);
}

void EditorApplication::draw_collider_wireframe(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                                const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::vec4 color = selected ? glm::vec4(1.00f, 0.75f, 0.35f, 1.0f) : glm::vec4(0.95f, 0.55f, 0.25f, 0.85f);
    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                      m_cubeIndexCount, viewProj * model_from_transform(t), color);
}

void EditorApplication::draw_entity_bounds(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                           UUID id, const TransformComponent& t) {
    (void)id;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                      m_cubeIndexCount, viewProj * model_from_transform(t),
                      glm::vec4(0.55f, 0.60f, 1.00f, 1.0f));
}

void EditorApplication::draw_gizmo_overlay(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_editorScene || !m_selectedEntity.is_valid()) return;
    const UUID id = m_selectedEntity.get_id();
    const auto it = m_editorScene->transformComponents.find(id);
    if (it == m_editorScene->transformComponents.end()) return;
    const glm::mat4 gizmoModel = glm::translate(glm::mat4(1.0f), it->second.position);

    const glm::vec4 highlight(1.0f, 0.85f, 0.30f, 1.0f);
    const glm::vec4 normal(1.0f);

    const VkDeviceSize zeroOffset = 0;
    const auto bind_gizmo = [&]() {
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_gizmoVB.buffer, &zeroOffset);
        vkCmdBindIndexBuffer(cmd, m_gizmoIB.buffer, 0, VK_INDEX_TYPE_UINT32);
    };

    // Solid pieces: arrow cones (translate) or tip cubes (scale).
    if (m_gizmoMode == GizmoMode::Translate || m_gizmoMode == GizmoMode::Scale) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gizmoPipeline);
        for (int axis = 0; axis < 3; ++axis) {
            const bool active = (m_activeAxis == static_cast<GizmoAxis>(axis + 1));
            const EditorApplication::GizmoDrawRange& range =
                (m_gizmoMode == GizmoMode::Translate) ? m_gizmoArrowRanges[axis] : m_gizmoTipRanges[axis];
            bind_gizmo();
            push_constants(cmd, m_scenePipelineLayout, viewProj * gizmoModel,
                           active ? highlight : normal);
            vkCmdDrawIndexed(cmd, range.count, 1, range.offset, 0, 0);
        }
    }

    // Wireframe pieces: shafts (translate/scale) or rings (rotate).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    for (int axis = 0; axis < 3; ++axis) {
        const bool active = (m_activeAxis == static_cast<GizmoAxis>(axis + 1));
        const EditorApplication::GizmoDrawRange& range =
            (m_gizmoMode == GizmoMode::Rotate) ? m_gizmoRingRanges[axis] : m_gizmoShaftRanges[axis];
        bind_gizmo();
        push_constants(cmd, m_scenePipelineLayout, viewProj * gizmoModel,
                       active ? highlight : normal);
        vkCmdDrawIndexed(cmd, range.count, 1, range.offset, 0, 0);
    }
}

void EditorApplication::render_pick_pass(VkCommandBuffer cmd) {
    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = m_offscreen.pickRenderPass;
    info.framebuffer = m_offscreen.pickFramebuffer;
    info.renderArea.offset = { 0, 0 };
    info.renderArea.extent = { m_offscreen.width, m_offscreen.height };
    VkClearValue clears[2];
    clears[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    info.clearValueCount = 2;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport_scissor(cmd, m_offscreen.width, m_offscreen.height);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pickPipeline);

    const float aspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();

    Scene* pickScene = m_playMode.get_active_scene();
    if (!pickScene) pickScene = m_editorScene.get();
    m_pickColorToEntity.clear();
    uint32_t nextId = 1;
    if (pickScene) {
        for (const auto& [id, ent] : pickScene->get_entities()) {
            const auto transformIt = pickScene->transformComponents.find(id);
            if (transformIt == pickScene->transformComponents.end()) continue;
            const uint32_t pickId = nextId++;
            m_pickColorToEntity[pickId] = id;
            const glm::vec4 color(
                static_cast<float>(pickId & 0xFF) / 255.0f,
                static_cast<float>((pickId >> 8) & 0xFF) / 255.0f,
                static_cast<float>((pickId >> 16) & 0xFF) / 255.0f,
                1.0f);
            bool drewMesh = false;
            const auto meshComp = pickScene->meshRendererComponents.find(id);
            if (meshComp != m_editorScene->meshRendererComponents.end() &&
                meshComp->second.meshAssetID.is_valid()) {
                if (const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID)) {
                    draw_mesh_resource(cmd, viewProj * model_from_transform(transformIt->second), color, *mesh);
                    drewMesh = true;
                }
            }
            if (!drewMesh) {
                draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                  m_cubeIndexCount, viewProj * model_from_transform(transformIt->second), color);
            }
        }
    }
    vkCmdEndRenderPass(cmd);
}

void EditorApplication::perform_pick_readback() {
    if (!m_editorScene || m_offscreen.framebuffer == VK_NULL_HANDLE || !m_pickPipeline) return;
    if (m_pickPixel.x < 0 || m_pickPixel.y < 0 ||
        m_pickPixel.x >= static_cast<float>(m_offscreen.width) ||
        m_pickPixel.y >= static_cast<float>(m_offscreen.height)) return;

    VkCommandBuffer cmd = begin_single_time_commands();
    render_pick_pass(cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { m_offscreen.width, m_offscreen.height, 1 };
    vkCmdCopyImageToBuffer(cmd, m_offscreen.pickImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_offscreen.pickStagingBuffer, 1, &region);
    end_single_time_commands(cmd);

    void* mapped = nullptr;
    vkMapMemory(m_device, m_offscreen.pickStagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
    const size_t x = static_cast<size_t>(m_pickPixel.x);
    const size_t y = static_cast<size_t>(m_pickPixel.y);
    const uint8_t* pixel = static_cast<const uint8_t*>(mapped) + (y * m_offscreen.width + x) * 4;
    const uint32_t id = static_cast<uint32_t>(pixel[0]) |
                        (static_cast<uint32_t>(pixel[1]) << 8) |
                        (static_cast<uint32_t>(pixel[2]) << 16);
    vkUnmapMemory(m_device, m_offscreen.pickStagingMemory);

    const auto found = m_pickColorToEntity.find(id);
    if (found != m_pickColorToEntity.end()) {
        m_selectedEntity = m_editorScene->find_entity_by_id(found->second);
        m_editorGui.select_entity(m_selectedEntity);
    }
}

// ===========================================================================
// Camera and gizmo interaction
// ===========================================================================

void EditorApplication::update_editor_camera(float deltaTime) {
    if (!m_viewportFocused) return;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    const glm::vec2 mouse(static_cast<float>(mx), static_cast<float>(my));
    const glm::vec2 mouseDelta = mouse - m_lastMousePos;
    m_lastMousePos = mouse;

    EditorCamera& cam = m_editorCamera;
    const glm::vec3 front = cam.get_front();
    const glm::vec3 right = cam.get_right();
    const glm::vec3 up = cam.get_up();

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

    // Fly (WASD) while orbiting.
    if (orbitHeld) {
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

    // Scroll zoom (handled here so it applies even without a hovered ImGui widget).
    if (m_viewportImageHovered) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            cam.orbitDistance = glm::clamp(cam.orbitDistance * (1.0f - io.MouseWheel * 0.1f), 0.5f, 5000.0f);
            io.MouseWheel = 0.0f;
        }
    }

    // Recompute the camera position from target + spherical offset.
    cam.position = cam.orbitTarget - euler_direction(cam.yaw, cam.pitch) * cam.orbitDistance;
}

void EditorApplication::process_viewport_input() {
    if (!m_viewportFocused) return;
    ImGuiIO& io = ImGui::GetIO();
    // Gizmo mode switching: W / E / R
    if (!io.WantCaptureKeyboard) {
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

bool EditorApplication::gizmo_axis_hit_test(glm::vec2 mouseScreen) {
    m_hoveredAxis = GizmoAxis::None;
    if (!m_editorScene || !m_selectedEntity.is_valid()) return false;
    const auto it = m_editorScene->transformComponents.find(m_selectedEntity.get_id());
    if (it == m_editorScene->transformComponents.end()) return false;
    const glm::vec3 origin = it->second.position;

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
                const glm::vec3 dir = kAxisDirs[axis];
                glm::vec3 u = glm::normalize(glm::cross(dir, std::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
                glm::vec3 v = glm::normalize(glm::cross(dir, u));
                const glm::vec3 p0 = origin + u * (std::cos(a0) * gizmoLen) + v * (std::sin(a0) * gizmoLen);
                const glm::vec3 p1 = origin + u * (std::cos(a1) * gizmoLen) + v * (std::sin(a1) * gizmoLen);
                dist = std::min(dist, dist_point_segment(mouseScreen, project(p0), project(p1)));
            }
        } else {
            const glm::vec2 tipScreen = project(origin + kAxisDirs[axis] * gizmoLen);
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
    m_gizmoAxisWorld = kAxisDirs[static_cast<int>(m_activeAxis) - 1];
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
            [this, id, newPos] { m_editorScene->transformComponents.at(id).position = newPos; },
            [this, id, start = m_gizmoDragEntityStart] {
                m_editorScene->transformComponents.at(id).position = start;
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
            [this, id, newRot] { m_editorScene->transformComponents.at(id).rotation = newRot; },
            [this, id, start = m_gizmoDragRotStart] {
                m_editorScene->transformComponents.at(id).rotation = start;
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
            [this, id, newScale] { m_editorScene->transformComponents.at(id).scale = newScale; },
            [this, id, start = m_gizmoDragScaleStart] {
                m_editorScene->transformComponents.at(id).scale = start;
            });
    }
}

// ===========================================================================
// Cooked mesh resources (real imported geometry in the viewport)
// ===========================================================================

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
    void* data = nullptr;
    vkMapMemory(m_device, resource.vb.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, resource.vb.memory);
    if (ibSize > 0) {
        vkMapMemory(m_device, resource.ib.memory, 0, ibSize, 0, &data);
        std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
        vkUnmapMemory(m_device, resource.ib.memory);
    }

    m_meshResources[assetId] = std::move(resource);
    return true;
}

const EditorApplication::EditorMeshResource* EditorApplication::get_mesh_resource(const UUID& assetId) {
    if (!assetId.is_valid()) return nullptr;
    if (!load_mesh_resource(assetId)) return nullptr;
    const auto found = m_meshResources.find(assetId);
    return (found != m_meshResources.end() && found->second.valid) ? &found->second : nullptr;
}

void EditorApplication::draw_mesh_resource(VkCommandBuffer cmd, const glm::mat4& mvp, const glm::vec4& color,
                                           const EditorMeshResource& resource) {
    if (!resource.valid || resource.vb.buffer == VK_NULL_HANDLE) return;
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &resource.vb.buffer, &offset);
    if (resource.ib.buffer != VK_NULL_HANDLE) {
        vkCmdBindIndexBuffer(cmd, resource.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    }
    push_constants(cmd, m_scenePipelineLayout, mvp, color);
    for (const EditorMeshResource::DrawRange& range : resource.ranges) {
        if (range.indexed) {
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
        } else {
            vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
        }
    }
}

bool EditorApplication::load_material_asset(const UUID& assetId) {
    if (!assetId.is_valid()) return false;
    if (m_materialAssets.contains(assetId)) return true;
    if (m_materialLoadFailed.contains(assetId)) return false;
    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Material || found->sourcePath.empty() ||
        !std::filesystem::is_regular_file(found->sourcePath)) {
        m_materialLoadFailed.insert(assetId);
        return false;
    }
    MaterialAsset mat;
    if (!mat.load_from_file(found->sourcePath)) {
        std::cerr << "[Editor] Cannot load material asset " << assetId.to_string() << std::endl;
        m_materialLoadFailed.insert(assetId);
        return false;
    }
    m_materialAssets[assetId] = std::move(mat);
    return true;
}

namespace {
// Decode a PNG payload via Windows Imaging Component into 8-bit RGBA.
bool decode_png_rgba(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba) {
    if (png.size() < 8) return false;
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) return false;
    }
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!hGlobal) return false;
    void* dst = GlobalLock(hGlobal);
    if (!dst) { GlobalFree(hGlobal); return false; }
    std::memcpy(dst, png.data(), png.size());
    GlobalUnlock(hGlobal);
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, stream.ReleaseAndGetAddressOf()))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return false;
    rgba.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()));
}
} // namespace

void EditorApplication::destroy_graph_texture(GraphTexture& t) {
    if (m_device == VK_NULL_HANDLE) return;
    if (t.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, t.view, nullptr);
    if (t.image != VK_NULL_HANDLE) vkDestroyImage(m_device, t.image, nullptr);
    if (t.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, t.memory, nullptr);
    t = GraphTexture{};
}

bool EditorApplication::load_viewport_texture(const UUID& assetId, GraphTexture& out, std::string& error) {
    const auto metaOpt = m_assetRegistry.find(assetId);
    if (!metaOpt) {
        error = "texture asset not found in registry";
        return false;
    }
    const AssetMetadata& meta = *metaOpt;
    if (meta.type != AssetType::Texture || meta.cookedPath.empty()) {
        error = "asset is not a cooked texture";
        return false;
    }
    std::ifstream in(meta.cookedPath, std::ios::binary);
    if (!in) {
        error = "cannot open cooked texture: " + meta.cookedPath.string();
        return false;
    }
    std::array<char, 5> magic{};
    in.read(magic.data(), magic.size());
    uint32_t version = 0, width = 0, height = 0, channels = 0;
    uint8_t bitDepth = 0; // the importer stores bitDepth as a single byte
    uint32_t mipCount = 1; // v2 = single level; v3 reads mipCount + flags
    uint8_t flags = 0;
    uint64_t payloadSize = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&width), sizeof(width));
    in.read(reinterpret_cast<char*>(&height), sizeof(height));
    in.read(reinterpret_cast<char*>(&channels), sizeof(channels));
    in.read(reinterpret_cast<char*>(&bitDepth), sizeof(bitDepth));
    if (version == 2) {
        mipCount = 1;
        flags = 0;
    } else if (version == 3) {
        in.read(reinterpret_cast<char*>(&mipCount), sizeof(mipCount));
        in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    }
    in.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));
    if (!in || std::string_view(magic.data(), magic.size()) != "VCTEX" ||
        (version != 2 && version != 3) || width == 0 || height == 0 || mipCount == 0 ||
        payloadSize == 0 || payloadSize > (1ull << 30)) {
        error = "invalid or unsupported VCTEX cooked texture (magic=" +
                std::string(magic.data(), magic.size()) + " version=" + std::to_string(version) +
                " size=" + std::to_string(width) + "x" + std::to_string(height) +
                " ch=" + std::to_string(channels) + " mips=" + std::to_string(mipCount) +
                " payload=" + std::to_string(payloadSize) +
                " path=" + meta.cookedPath.string() + " fileSize=" +
                std::to_string(std::filesystem::file_size(meta.cookedPath)) + ")";
        return false;
    }
    std::vector<uint8_t> payload(static_cast<size_t>(payloadSize));
    in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payloadSize));
    if (!in) {
        error = "truncated VCTEX payload";
        return false;
    }
    std::vector<uint8_t> rgba;
    const bool isPng = payload.size() >= 8 &&
                       std::memcmp(payload.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    if (isPng) {
        if (!decode_png_rgba(payload, rgba)) {
            error = "PNG decode failed (WIC)";
            return false;
        }
        // PNG stays a single level (raw payload); srgb is still applied.
        return upload_texture_pixels(width, height, rgba, 1, (flags & 1u) != 0, out, error);
    }
    // TGA/HDR importers store decoded pixels in the payload. Radiance HDR
    // (bitDepth 32, channels 4) stores RGBA16F half-float pairs (w*h*8 bytes)
    // and is uploaded as an R16G16B16A16_SFLOAT image; TGA stores 8-bit
    // RGB/RGBA (w*h*3/4 bytes per level, mip chain when mipCount > 1).
    if (bitDepth == 32 && channels == 4 &&
        payload.size() == static_cast<size_t>(width) * height * 8) {
        return upload_texture_half_pixels(width, height, payload, out, error);
    }
    uint64_t expectedTotal = 0;
    for (uint32_t m = 0; m < mipCount; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        expectedTotal += static_cast<uint64_t>(mw) * mh * channels;
    }
    if (payload.size() != expectedTotal) {
        error = "unsupported cooked texture payload layout (expected " +
                std::to_string(expectedTotal) + " bytes, got " + std::to_string(payload.size()) +
                " mips=" + std::to_string(mipCount) + ")";
        return false;
    }
    rgba.reserve(static_cast<size_t>(expectedTotal) / channels * 4);
    size_t offset = 0;
    for (uint32_t m = 0; m < mipCount; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        const size_t levelBytes = static_cast<size_t>(mw) * mh * channels;
        const uint8_t* level = payload.data() + offset;
        if (channels == 4) {
            rgba.insert(rgba.end(), level, level + levelBytes);
        } else if (channels == 3) {
            for (size_t i = 0; i < levelBytes; i += 3) {
                rgba.push_back(level[i]);
                rgba.push_back(level[i + 1]);
                rgba.push_back(level[i + 2]);
                rgba.push_back(255);
            }
        } else {
            error = "unsupported cooked texture channel count";
            return false;
        }
        offset += levelBytes;
    }
    return upload_texture_pixels(width, height, rgba, mipCount, (flags & 1u) != 0, out, error);
}

bool EditorApplication::upload_texture_pixels(uint32_t width, uint32_t height,
                                              const std::vector<uint8_t>& rgba,
                                              uint32_t mipCount, bool srgb,
                                              GraphTexture& out, std::string& error) {
    // Import settings applied here (Fase 2): srgb selects the SRGB image
    // format and mipCount uploads the cooked mip chain (level 0 first) into a
    // mip-mapped image + view, so mipmapped textures actually sample the chain.
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    out.format = format;
    const uint32_t mips = std::max(mipCount, 1u);
    VkDeviceSize imageSize = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        imageSize += static_cast<VkDeviceSize>(mw) * mh * 4;
    }
    create_image(width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, out.memory, mips);
    if (out.image == VK_NULL_HANDLE) {
        error = "texture image allocation failed";
        return false;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "texture staging buffer allocation failed";
        return false;
    }
    void* data = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &data);
    std::memcpy(data, rgba.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mips);
    VkDeviceSize offset = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
        region.imageExtent = { mw, mh, 1 };
        regions.push_back(region);
        offset += static_cast<VkDeviceSize>(mw) * mh * 4;
    }
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);
    vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);
    end_single_time_commands(cmd);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    out.view = create_image_view(out.image, format, VK_IMAGE_ASPECT_COLOR_BIT, mips);
    if (out.view == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "texture image view creation failed";
        return false;
    }
    return true;
}

// Uploads an RGBA16F (half-float RGBA) payload as an R16G16B16A16_SFLOAT image
// — the HDR path of the material graph (Radiance .hdr cooks to this layout).
bool EditorApplication::upload_texture_half_pixels(uint32_t width, uint32_t height,
                                                   const std::vector<uint8_t>& halfRgba,
                                                   GraphTexture& out, std::string& error) {
    const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    out.format = format;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 8;
    create_image(width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, out.memory);
    if (out.image == VK_NULL_HANDLE) {
        error = "HDR texture image allocation failed";
        return false;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "HDR texture staging buffer allocation failed";
        return false;
    }
    void* data = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &data);
    std::memcpy(data, halfRgba.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    out.view = create_image_view(out.image, format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (out.view == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "HDR texture image view creation failed";
        return false;
    }
    return true;
}

bool EditorApplication::build_graph_pipeline(const Rendering::MaterialGraph& graph, GraphMaterialPipeline& out) {
    // Preserve the caller's cache key: out is reset below and graphHash is the
    // rebuild-detection stamp used by the per-material cache.
    const uint64_t callerGraphHash = out.graphHash;
    out = GraphMaterialPipeline{};
    out.graphHash = callerGraphHash;
    // TextureSample nodes: the texture asset UUID lives in the node value
    // (string). Binding i+1 corresponds to the i-th TextureSample in node order.
    std::vector<UUID> textureIds;
    for (const auto& node : graph.nodes()) {
        if (node.kind != Rendering::MaterialNodeKind::TextureSample) continue;
        const auto* value = std::get_if<std::string>(&node.value);
        textureIds.push_back(value && !value->empty() ? UUID::from_string(*value) : UUID{});
    }
    std::vector<GraphTexture> textures;
    textures.reserve(textureIds.size());
    for (const UUID& id : textureIds) {
        GraphTexture tex;
        std::string texError;
        if (!id.is_valid() || !load_viewport_texture(id, tex, texError)) {
            out.lastError = texError.empty()
                ? "a TextureSample node has no texture asset assigned" : texError;
            for (auto& t : textures) destroy_graph_texture(t);
            return false;
        }
        textures.push_back(std::move(tex));
    }
    out.textures = std::move(textures);
    const Rendering::GlslGenerationResult gen = material_graph_to_glsl(graph);
    if (!gen) {
        out.lastError = gen.errors.empty() ? "material graph compile failed" : gen.errors[0].message;
        return false;
    }

    const std::vector<uint32_t> vertSpv = read_spv("editor_material.vert.spv");
    if (vertSpv.empty()) {
        out.lastError = "editor_material.vert.spv is missing (re-run compile_shaders)";
        return false;
    }
    const std::vector<uint32_t> fragSpv = compile_material_glsl(VK_SHADER_STAGE_FRAGMENT_BIT, gen.source);
    if (fragSpv.empty()) {
        out.lastError = "glslc failed to compile the generated material shader";
        return false;
    }
    VkShaderModule vertModule = make_module(m_device, vertSpv);
    VkShaderModule fragModule = make_module(m_device, fragSpv);
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, fragModule, nullptr);
        out.lastError = "VkShaderModule creation failed";
        return false;
    }

    // Descriptor set layout: binding 0 = material params UBO; bindings 1..N =
    // combined image samplers (one per TextureSample node, node order); then the
    // LightParams UBO and the shadow-map sampler at the bindings the generated
    // shader declared.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(3 + out.textures.size());
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(uboBinding);
    for (size_t i = 0; i < out.textures.size(); ++i) {
        VkDescriptorSetLayoutBinding texBinding{};
        texBinding.binding = static_cast<uint32_t>(i + 1);
        texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texBinding.descriptorCount = 1;
        texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(texBinding);
    }
    out.lightUboBinding = gen.lightUboBinding;
    VkDescriptorSetLayoutBinding lightBinding{};
    lightBinding.binding = out.lightUboBinding;
    lightBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightBinding.descriptorCount = 1;
    lightBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(lightBinding);

    // Shadow-map sampler (dummy 1x1 white texture; shadows disabled in the
    // editor viewport).
    out.shadowSamplerBinding = gen.shadowSamplerBinding;
    {
        std::string texError;
        if (!upload_texture_pixels(1, 1, { 255, 255, 255, 255 }, 1, false, out.shadowDummy, texError)) {
            destroy_graph_pipeline(out);
            out.lastError = "shadow dummy texture failed: " + texError;
            return false;
        }
    }
    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = out.shadowSamplerBinding;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(shadowBinding);
    VkDescriptorSetLayoutCreateInfo dslInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    dslInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &dslInfo, nullptr, &out.descriptorSetLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        destroy_graph_pipeline(out);
        out.lastError = "descriptor set layout creation failed";
        return false;
    }

    // Pipeline layout: MVP + model push constant (vertex stage) + material set
    // (params + textures + lights, all fragment).
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(Rendering::MaterialPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &out.descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &out.layout) != VK_SUCCESS) {
        destroy_graph_pipeline(out);
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        out.lastError = "pipeline layout creation failed";
        return false;
    }

    // UBO sized with std140 offsets; capture parameter defaults for per-frame writes.
    out.uniformNames = gen.uniformNames;
    out.uniformTypes = gen.uniformTypes;
    out.uniformDefaults.reserve(gen.uniformNames.size());
    out.uboSize = 0;
    for (size_t i = 0; i < gen.uniformNames.size(); ++i) {
        const auto* parameter = graph.find_parameter(gen.uniformNames[i]);
        out.uniformDefaults.push_back(parameter ? parameter->defaultValue : Rendering::MaterialValue(0.0f));
        out.uboSize = align_material_offset(out.uboSize, material_std140_alignment(gen.uniformTypes[i]));
        out.uboSize += material_std140_size(gen.uniformTypes[i]);
    }
    out.uboSize = align_material_offset(out.uboSize, 16);
    if (out.uboSize == 0) out.uboSize = 16;
    create_buffer(out.uboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  out.uboBuffer, out.uboMemory);
    create_buffer(sizeof(Rendering::LightUboData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  out.lightBuffer, out.lightMemory);

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(out.textures.size()) + 1;
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    bool poolOk = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &out.pool) == VK_SUCCESS;
    if (poolOk) {
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = out.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &out.descriptorSetLayout;
        poolOk = vkAllocateDescriptorSets(m_device, &allocInfo, &out.descriptorSet) == VK_SUCCESS;
    }
    if (!poolOk) {
        out.lastError = "descriptor pool/set allocation failed";
        destroy_graph_pipeline(out);
        return false;
    }
    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(out.textures.size() + 2);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(3 + out.textures.size());
    VkDescriptorBufferInfo bufferInfo{ out.uboBuffer, 0, out.uboSize };
    VkWriteDescriptorSet uboWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    uboWrite.dstSet = out.descriptorSet;
    uboWrite.descriptorCount = 1;
    uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboWrite.pBufferInfo = &bufferInfo;
    writes.push_back(uboWrite);
    VkDescriptorBufferInfo lightBufferInfo{ out.lightBuffer, 0, sizeof(Rendering::LightUboData) };
    VkWriteDescriptorSet lightWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    lightWrite.dstSet = out.descriptorSet;
    lightWrite.dstBinding = out.lightUboBinding;
    lightWrite.descriptorCount = 1;
    lightWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightWrite.pBufferInfo = &lightBufferInfo;
    writes.push_back(lightWrite);
    for (size_t i = 0; i < out.textures.size(); ++i) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = m_offscreen.sampler;
        imageInfo.imageView = out.textures[i].view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.push_back(imageInfo);
        VkWriteDescriptorSet texWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        texWrite.dstSet = out.descriptorSet;
        texWrite.dstBinding = static_cast<uint32_t>(i + 1);
        texWrite.descriptorCount = 1;
        texWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texWrite.pImageInfo = &imageInfos.back();
        writes.push_back(texWrite);
    }
    VkDescriptorImageInfo shadowImageInfo{};
    // Real sun shadow map when available; the 1x1 dummy otherwise (no sun).
    shadowImageInfo.sampler = m_shadowMap.sampler != VK_NULL_HANDLE
        ? m_shadowMap.sampler : m_offscreen.sampler;
    shadowImageInfo.imageView = m_shadowMap.view != VK_NULL_HANDLE
        ? m_shadowMap.view : out.shadowDummy.view;
    shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos.push_back(shadowImageInfo);
    VkWriteDescriptorSet shadowWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    shadowWrite.dstSet = out.descriptorSet;
    shadowWrite.dstBinding = out.shadowSamplerBinding;
    shadowWrite.descriptorCount = 1;
    shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowWrite.pImageInfo = &imageInfos.back();
    writes.push_back(shadowWrite);
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // Graphics pipeline: same EditorVertex layout, no culling (glTF winding varies).
    out.pipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, out.layout,
                                         vertModule, fragModule, false, true, false, true);
    vkDestroyShaderModule(m_device, vertModule, nullptr);
    vkDestroyShaderModule(m_device, fragModule, nullptr);
    if (out.pipeline == VK_NULL_HANDLE) {
        out.lastError = "vkCreateGraphicsPipelines failed";
        destroy_graph_pipeline(out);
        return false;
    }
    out.lastError.clear();
    out.valid = true;
    return true;
}

void EditorApplication::destroy_graph_pipeline(GraphMaterialPipeline& p) {
    if (m_device == VK_NULL_HANDLE) return;
    if (p.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, p.pipeline, nullptr);
    if (p.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, p.layout, nullptr);
    if (p.descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, p.descriptorSetLayout, nullptr);
    if (p.pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, p.pool, nullptr);
    if (p.uboBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, p.uboBuffer, nullptr);
    if (p.uboMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, p.uboMemory, nullptr);
    if (p.lightBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, p.lightBuffer, nullptr);
    if (p.lightMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, p.lightMemory, nullptr);
    destroy_graph_texture(p.shadowDummy);
    for (auto& t : p.textures) destroy_graph_texture(t);
    p.textures.clear();
    p = GraphMaterialPipeline{};
}

void EditorApplication::destroy_graph_material_pipelines() {
    for (auto& [id, p] : m_graphMaterialPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_graphMaterialPipelines.clear();
    destroy_graph_pipeline(m_liveGraphPipeline);
    m_liveGraphHash = 0;
}

void EditorApplication::write_light_ubo(GraphMaterialPipeline& p, const Scene* scene,
                                        const glm::vec3& cameraPos) {
    if (!p.valid || p.lightBuffer == VK_NULL_HANDLE) return;
    Rendering::LightUboData data{};
    data.cameraPosition = glm::vec4(cameraPos, 1.0f);
    // Real sun shadow map: VP + enabled flag + bias + single-map mode (z=1).
    data.sunViewProj = m_shadowMap.viewProj;
    data.shadowParams = glm::vec4(m_shadowMap.enabled ? 1.0f : 0.0f, 0.0006f,
                                  m_shadowMap.enabled ? 1.0f : 0.0f, 0.0f);
    const glm::mat4 view = m_editorCamera.get_view_matrix();
    data.cameraForward = glm::vec4(glm::normalize(glm::vec3(-view[2][0], -view[2][1], -view[2][2])), 0.0f);
    uint32_t pointCount = 0, spotCount = 0, areaCount = 0;
    if (scene) {
        for (const auto& [id, light] : scene->lightComponents) {
            glm::vec3 dir(0.0f, -1.0f, 0.0f);
            glm::vec3 position(0.0f);
            const auto tit = scene->transformComponents.find(id);
            if (tit != scene->transformComponents.end()) {
                position = tit->second.position;
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                dir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            const glm::vec3 colorIntensity = light.color * light.intensity;
            if (is_directional_sun(light)) {
                data.sunDirection = glm::vec4(dir, 1.0f);
                data.sunColor = glm::vec4(colorIntensity, 1.0f);
            } else if (light.type == LightType::Spot && spotCount < Rendering::kMaxSpotLights) {
                data.spotLightPos[spotCount] = glm::vec4(position, light.range);
                data.spotLightDir[spotCount] = glm::vec4(dir, 1.0f);
                data.spotLightParams[spotCount] = glm::vec4(
                    std::cos(glm::radians(25.0f)), std::cos(glm::radians(45.0f)), 0.0f, 0.0f);
                data.spotLightColor[spotCount] = glm::vec4(colorIntensity, 1.0f);
                ++spotCount;
            } else if (light.type == LightType::Area && areaCount < Rendering::kMaxAreaLights) {
                data.areaLightPos[areaCount] = glm::vec4(position, 1.0f);
                data.areaLightNormal[areaCount] = glm::vec4(dir, 1.0f);
                data.areaLightHalf[areaCount] = glm::vec4(2.0f, 1.0f, 0.0f, 0.0f);
                data.areaLightColor[areaCount] = glm::vec4(colorIntensity, 1.0f);
                ++areaCount;
            } else if (pointCount < Rendering::kMaxPointLights) {
                data.pointLightPos[pointCount] = glm::vec4(position, light.range);
                data.pointLightColor[pointCount] = glm::vec4(colorIntensity, 1.0f);
                ++pointCount;
            }
        }
    }
    void* mapped = nullptr;
    if (vkMapMemory(m_device, p.lightMemory, 0, sizeof(data), 0, &mapped) != VK_SUCCESS) return;
    std::memcpy(mapped, &data, sizeof(data));
    vkUnmapMemory(m_device, p.lightMemory);
}

void EditorApplication::write_material_ubo(const GraphMaterialPipeline& p, const MaterialAsset* material,
                                           const MaterialComponent* component) {
    if (!p.valid || p.uboBuffer == VK_NULL_HANDLE || p.uboSize == 0) return;
    const glm::vec3 albedo = material ? material->albedo
                                      : (component ? component->albedo : glm::vec3(1.0f, 1.0f, 1.0f));
    const float roughness = material ? material->roughness : (component ? component->roughness : 0.5f);
    const float metallic = material ? material->metallic : (component ? component->metallic : 0.0f);
    const glm::vec3 emissive = material
        ? material->emissiveColor * material->emissiveIntensity
        : (component ? component->emissiveColor * component->emissiveIntensity : glm::vec3(0.0f));
    const float emissiveIntensity = material ? material->emissiveIntensity
                                             : (component ? component->emissiveIntensity : 0.0f);
    void* mapped = nullptr;
    if (vkMapMemory(m_device, p.uboMemory, 0, p.uboSize, 0, &mapped) != VK_SUCCESS) return;
    auto* bytes = static_cast<std::byte*>(mapped);
    size_t offset = 0;
    for (size_t i = 0; i < p.uniformNames.size(); ++i) {
        offset = align_material_offset(offset, material_std140_alignment(p.uniformTypes[i]));
        if (offset + 16 > p.uboSize) break;
        Rendering::MaterialValue value = p.uniformDefaults[i];
        const std::string& name = p.uniformNames[i];
        if (name == "Albedo" || name == "BaseColor") {
            value = (p.uniformTypes[i] == Rendering::MaterialValueType::Vec4)
                ? Rendering::MaterialValue(glm::vec4(albedo, 1.0f))
                : Rendering::MaterialValue(albedo);
        } else if (name == "Roughness") {
            value = roughness;
        } else if (name == "Metallic") {
            value = metallic;
        } else if (name == "Emissive") {
            value = (p.uniformTypes[i] == Rendering::MaterialValueType::Vec4)
                ? Rendering::MaterialValue(glm::vec4(emissive, 1.0f))
                : Rendering::MaterialValue(emissive);
        } else if (name == "EmissiveIntensity") {
            value = emissiveIntensity;
        } else if (name == "Opacity") {
            value = 1.0f;
        }
        write_ubo_value(bytes + offset, p.uniformTypes[i], value);
        offset += material_std140_size(p.uniformTypes[i]);
    }
    vkUnmapMemory(m_device, p.uboMemory);
}

void EditorApplication::destroy_mesh_resources() {
    for (auto& [id, resource] : m_meshResources) {
        (void)id;
        if (resource.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, resource.vb.buffer, nullptr);
        if (resource.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, resource.vb.memory, nullptr);
        if (resource.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, resource.ib.buffer, nullptr);
        if (resource.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, resource.ib.memory, nullptr);
    }
    m_meshResources.clear();
    m_meshLoadFailed.clear();
}

void EditorApplication::cleanup() {
#ifdef _WIN32
    stop_external_game();
#endif
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        destroy_mesh_resources();
        destroy_graph_material_pipelines();
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

        const auto destroy_buffer = [&](GPUBuffer& buffer) {
            if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, buffer.buffer, nullptr);
            if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, buffer.memory, nullptr);
            buffer = GPUBuffer{};
        };
        destroy_buffer(m_cubeVB);
        destroy_buffer(m_cubeIB);
        destroy_buffer(m_gridVB);
        destroy_buffer(m_lightIconVB);
        destroy_buffer(m_cameraIconVB);
        destroy_buffer(m_gizmoVB);
        destroy_buffer(m_gizmoIB);

        if (m_pickPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_pickPipeline, nullptr); m_pickPipeline = VK_NULL_HANDLE; }
        if (m_gizmoPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_gizmoPipeline, nullptr); m_gizmoPipeline = VK_NULL_HANDLE; }
        if (m_wireframePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_wireframePipeline, nullptr); m_wireframePipeline = VK_NULL_HANDLE; }
        if (m_scenePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_scenePipeline, nullptr); m_scenePipeline = VK_NULL_HANDLE; }
        if (m_scenePipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_scenePipelineLayout, nullptr); m_scenePipelineLayout = VK_NULL_HANDLE; }
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
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        vkDestroyInstance(m_instance, nullptr);

        if (m_window) {
            glfwDestroyWindow(m_window);
            glfwTerminate();
        }
    }
}

} // namespace Engine
