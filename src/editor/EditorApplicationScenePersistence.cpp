// EditorApplicationScenePersistence.cpp
//
// Agente 3 (fechamento_solidacao) — TU split: cohesive scene/settings/terrain methods were
// extracted verbatim from the 209KB EditorApplicationRecovered.cpp (behavior
// preserved; CMake still compiles this TU into VulkanEngineEditor).
#include "EditorApplication.hpp"
#include "EditorInternalHelpers.hpp"
#include "EditorApplicationRecoveredShared.hpp"
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
    // Theme colors are parsed and reapplied on boot when present in the
    // settings: the Forge light design system is the base theme, and the
    // Theme Editor panel tunes the live style during the session (applied
    // here via set_theme, so a saved theme survives a restart).
    const glm::vec3 bg = findVec("themeBg");
    const glm::vec3 panel = findVec("themePanel");
    if (bg.x >= 0.0f && panel.x >= 0.0f) {
        m_wickedTools.set_theme(bg, panel);
    }
    std::cout << "[Editor] Configurações carregadas: " << m_settingsPath << std::endl;
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
// in any revision). REAL implementations using existing editor state — the
// SDK contracts are instantiated in the constructor (agente 4 D.4) and these
// refreshers mirror the LIVE editor state into them each frame, so every
// GET endpoint returns real observable JSON instead of {"valid":false}.
// ---------------------------------------------------------------------------




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


}  // namespace Engine
