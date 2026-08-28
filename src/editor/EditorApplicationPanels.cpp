#include <imgui.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

// ForgeTheme + ForgeWidgets: define the colors inline here to avoid the
// transitive brace-mismatch that drags through Scene.hpp → Entity.hpp when
// ForgeWidgets.hpp is included after EditorApplication.hpp.
namespace Engine::UI {
struct Colors {
    static constexpr ImVec4 Background    { 0.06f, 0.07f, 0.09f, 1.0f };
    static constexpr ImVec4 Surface       { 0.10f, 0.11f, 0.14f, 1.0f };
    static constexpr ImVec4 SurfaceAlt    { 0.14f, 0.15f, 0.19f, 1.0f };
    static constexpr ImVec4 Border        { 0.22f, 0.24f, 0.30f, 1.0f };
    static constexpr ImVec4 Text          { 0.92f, 0.94f, 0.98f, 1.0f };
    static constexpr ImVec4 TextSecondary { 0.70f, 0.74f, 0.82f, 1.0f };
    static constexpr ImVec4 TextMuted     { 0.52f, 0.56f, 0.64f, 1.0f };
    static constexpr ImVec4 Accent        { 0.32f, 0.55f, 1.00f, 1.0f };
    static constexpr ImVec4 AccentHover   { 0.40f, 0.62f, 1.00f, 1.0f };
    static constexpr ImVec4 AccentSoft    { 0.17f, 0.23f, 0.34f, 1.0f };
    static constexpr ImVec4 Success       { 0.17f, 0.72f, 0.40f, 1.0f };
    static constexpr ImVec4 Warning       { 0.92f, 0.63f, 0.15f, 1.0f };
    static constexpr ImVec4 Danger        { 0.90f, 0.28f, 0.30f, 1.0f };
};
bool sectionHeader(const char* icon, const char* title, bool defaultOpen = true);
void propertyLabel(const char* label);
bool beginPropertyRow(const char* label);
void endPropertyRow(bool table);
bool vec3Property(const char* label, float* values, float speed = 0.1f);
bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool successButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool iconButton(const char* icon, const char* tooltip, bool selected = false);
bool toggle(const char* id, bool* value, const char* tooltip = nullptr);
void beginCard(const char* id);
void endCard();

}
using Engine::UI::sectionHeader;
using Engine::UI::propertyLabel;
using Engine::UI::beginPropertyRow;
using Engine::UI::endPropertyRow;
using Engine::UI::vec3Property;
using Engine::UI::primaryButton;
using Engine::UI::successButton;
using Engine::UI::iconButton;
using Engine::UI::toggle;
using Engine::UI::beginCard;
using Engine::UI::endCard;
// entityIcon is declared at global scope (see forward decl above) to avoid
// the ForgeWidgets.hpp brace-mismatch.

// IconsFontAwesome6.h is just #define macros — no braces.
#include "frontend/IconsFontAwesome6.h"

// Forward-declare the Engine namespace and key types so entityIcon's
// signature can reference them without dragging Scene.hpp (which would
// reintroduce the brace-mismatch).
namespace Engine { class Scene; class UUID; }
const char* entityIcon(Engine::Scene* scene, const Engine::UUID& id);

#include "EditorApplication.hpp"
#include <imgui_internal.h>
#include "EditorInternalHelpers.hpp"

namespace Engine {

// ===========================================================================
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

    // Scan Projects/ for real project folders (no hardcoded list).
    std::vector<LauncherProject> projects;
    scan_projects(projects);

    if (projects.empty()) {
        ImGui::TextDisabled("%s", tr("Nenhum projeto encontrado em Projects/ — crie um novo acima.",
                                      "No projects found in Projects/ — create one above."));
        ImGui::Spacing();
    }

    for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
        const auto& proj = projects[i];
        bool isSelected = (m_selectedProjectIndex == i);

        ImGui::PushID(i);
        if (ImGui::Selectable("##ProjectSelectable", isSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 80))) {
            m_selectedProjectIndex = i;
            m_currentProjectName = proj.name;
            if (ImGui::IsMouseDoubleClicked(0)) {
                m_inLauncherMode = false; // Launch Engine Studio
                glfwSetWindowTitle(m_window, ("VulkanCraft Engine - [" + m_currentProjectName + "]").c_str());
            }
        }
        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::TextColored(isSelected ? ImVec4(0.4f, 0.7f, 1.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "[JOGO]  %s", proj.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(proj.hasScene ? ImVec4(0.20f, 0.82f, 0.60f, 1.0f) : ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
                           proj.hasScene ? tr("[TEM CENA]", "[HAS SCENE]") : tr("[VAZIO]", "[EMPTY]"));

        ImGui::TextDisabled("Pasta: %s", proj.path.c_str());
        ImGui::TextDisabled("%s: %s", tr("Modificado", "Last modified"), proj.lastModified.c_str());
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
        glfwSetWindowTitle(m_window, ("VulkanCraft Engine - [" + m_currentProjectName + "]").c_str());
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    if (ImGui::Button(tr("+ Criar Novo Jogo", "+ Create New Game"), ImVec2(220, 44))) {
        m_inLauncherMode = false;
        glfwSetWindowTitle(m_window, "VulkanCraft Engine - [Novo Jogo]");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Procurar Pasta...", "Browse Folder..."), ImVec2(220, 44))) {
        std::string folder;
        if (pick_folder_dialog(folder, L"Escolher pasta do projeto")) {
            // Enter the editor scoped to the chosen project folder.
            m_currentProjectName = std::filesystem::path(folder).filename().string();
            if (m_currentProjectName.empty()) m_currentProjectName = "Projeto";
            m_inLauncherMode = false;
            glfwSetWindowTitle(m_window, ("VulkanCraft Engine - [" + m_currentProjectName + "]").c_str());
        }
    }

    ImGui::End();
}

void EditorApplication::draw_dockspace() {
    static bool firstTime = true;
    ImGuiID dockspace_id = ImGui::GetID("VulkanEngineStudioDockspace");
    // Persisted layouts are applied only after the dockspace exists. This keeps
    // ImGui's builder authoritative for the first frame and makes reset/load
    // affect real dock nodes instead of only the JSON model.
    if (firstTime) {
        apply_layout_snapshot_to_imgui();
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Dock area starts below the main menu bar + the 56 px app bar. Base the
    // math on viewport->Pos/Size (not WorkPos/WorkSize, which already accounts
    // for the menu bar) so the offset is applied exactly once.
    const float menuBarHeight = ImGui::GetFrameHeight();
    const float appBarHeight = 56.0f;
    ImVec2 dockPos = ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight + appBarHeight);
    ImVec2 dockSize = ImVec2(viewport->Size.x, viewport->Size.y - menuBarHeight - appBarHeight);

    ImGui::SetNextWindowPos(dockPos);
    ImGui::SetNextWindowSize(dockSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // No global WindowMinSize may inflate the shell (it fills the remaining area).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));

    ImGui::Begin("VulkanCraft Engine Shell", nullptr, host_window_flags);
    ImGui::PopStyleVar(4);

    ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    if (firstTime) {
        firstTime = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, dockspace_flags | ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, dockSize);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.20f, nullptr, &dock_main_id);
        // The left column is split vertically: Scene (always visible) on top,
        // the voxel sculpting tools below it — never a competing tab that hides
        // the scene hierarchy.
        ImGuiID dock_left_bottom = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.30f, nullptr, &dock_left);
        ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.25f, nullptr, &dock_main_id);
        ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down, 0.25f, nullptr, &dock_main_id);

        ImGui::DockBuilderDockWindow(tr("Cena", "Scene"), dock_left);
        ImGui::DockBuilderDockWindow(tr("Inspector", "Inspector"), dock_right);
        ImGui::DockBuilderDockWindow(tr("Viewport", "Viewport"), dock_main_id);
        ImGui::DockBuilderDockWindow(tr("Assets", "Assets"), dock_bottom);
        ImGui::DockBuilderDockWindow(tr("Console", "Console"), dock_bottom);
#if VC_ENABLE_VOXEL_PLUGIN
        ImGui::DockBuilderDockWindow(tr("Escultura de Blocos", "Voxel Sculpting Tools"), dock_left_bottom);
#endif

        ImGui::DockBuilderFinish(dockspace_id);
    }
    // The public layout model is the source of truth for visibility. Apply it
    // every frame so changes made by the Layout Settings panel affect the
    // actual dock windows, not only the serialized model.
    const auto setWindowVisible = [](const char* name, bool visible) {
        if (ImGuiWindow* window = ImGui::FindWindowByName(name))
            window->Hidden = !visible;
    };
    setWindowVisible(tr("Cena", "Scene"), m_showHierarchy);
    setWindowVisible(tr("Inspector", "Inspector"), m_showInspector);
    setWindowVisible(tr("Viewport", "Viewport"), m_showViewport);
    setWindowVisible(tr("Assets", "Assets"), m_showContentBrowser);
    setWindowVisible(tr("Console", "Console"), m_showConsole);
    ImGui::End();
}

void EditorApplication::draw_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(tr("Arquivo", "File"))) {
            if (ImGui::MenuItem(tr("Gerenciador de Jogos", "Game Launcher Hub"))) {
                m_inLauncherMode = true;
                glfwSetWindowTitle(m_window, tr("VulkanCraft Engine - Gerenciador de Jogos", "VulkanCraft Engine - Game Launcher"));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Novo Jogo", "New Scene"), "Ctrl+N")) {
                // Ask whether to save the current scene before discarding it.
                m_pendingNewSceneConfirm = true;
            }
            if (ImGui::MenuItem(tr("Abrir Jogo...", "Open Scene..."), "Ctrl+O")) {
                std::string scenePath;
                if (pick_file_dialog(scenePath, L"Cenas VulkanCraft (*.scene)\0*.scene\0Todos (*.*)\0*.*\0",
                                     L"Abrir Cena", L"scene")) {
                    load_scene_file(scenePath);
                }
            }
            if (ImGui::MenuItem(tr("Salvar Jogo", "Save Scene"), "Ctrl+S")) {
                save_current_scene();
            }
            if (ImGui::MenuItem(tr("Salvar Como...", "Save Scene As..."))) {
                save_scene_as();
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
                mark_scene_dirty();
            }
            if (ImGui::MenuItem(tr("Refazer", "Redo"), "Ctrl+Y", false, m_undo.can_redo())) {
                m_undo.redo();
                mark_scene_dirty();
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
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Objeto 3D > Cubo", "3D Object > Cube"))) {
                if (m_editorScene) {
                    Entity cube = m_editorScene->create_entity(tr("Cubo 3D", "Cube"));
                    m_editorScene->meshRendererComponents[cube.get_id()] = MeshRendererComponent{};
                    m_selectedEntity = cube;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz do Sol", "Light > Directional Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz do Sol", "Directional Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{};
                    m_selectedEntity = light;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz de Lâmpada", "Light > Point Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz de Lâmpada", "Point Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(1.0f, 0.8f, 0.4f), 5000.0f, 15.0f, true };
                    m_selectedEntity = light;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz Spot", "Light > Spot Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz Spot", "Spot Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot };
                    m_selectedEntity = light;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz de Área", "Light > Area Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz de Área", "Area Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area };
                    m_selectedEntity = light;
                    mark_scene_dirty();
                }
            }
#if VC_ENABLE_VOXEL_PLUGIN
            if (ImGui::MenuItem(tr("Blocos > Mundo de Blocos", "Voxel > Voxel Terrain Volume"))) {
                if (m_editorScene) {
                    Entity voxel = m_editorScene->create_entity(tr("Mundo de Blocos", "Voxel Volume"));
                    m_editorScene->voxelVolumeComponents[voxel.get_id()] = VoxelVolumeComponent{};
                    m_selectedEntity = voxel;
                    mark_scene_dirty();
                }
            }
#endif
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Janelas", "Window"))) {
            ImGui::MenuItem(tr("Viewport", "Viewport"), nullptr, &m_showViewport);
            ImGui::MenuItem(tr("Cena", "Scene"), nullptr, &m_showHierarchy);
            ImGui::MenuItem(tr("Inspector", "Inspector"), nullptr, &m_showInspector);
            ImGui::MenuItem(tr("Assets", "Assets"), nullptr, &m_showContentBrowser);
            ImGui::MenuItem(tr("UI de Gameplay", "Gameplay UI"), nullptr, &m_showGameplayUi);
#if VC_ENABLE_VOXEL_PLUGIN
            ImGui::MenuItem(tr("Escultura de Blocos", "Voxel Sculpting Tools"), nullptr, &m_showVoxelTools);
#endif
                    ImGui::MenuItem(tr("Console", "Console"), nullptr, &m_showConsole);
            ImGui::MenuItem(tr("Render Debugger", "Render Debugger"), nullptr, &m_showRenderDebugger);
            ImGui::MenuItem(tr("Layout Settings", "Layout Settings"), nullptr, &m_showLayoutSettings);
            ImGui::MenuItem(tr("Debugger de Scripts", "Script Debugger"), nullptr, &m_showScriptDebugger);
            ImGui::MenuItem(tr("Canvas de Scripts", "Script Canvas"), nullptr, &m_showScriptCanvas);
            ImGui::Separator();
            ImGui::MenuItem(tr("Editores Especializados", "Specialized Editors"), nullptr, &m_specializedEditors.open);
            m_wickedTools.draw_tools_menu();
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Salvar Layout", "Save Layout"))) save_layout_settings();
            if (ImGui::MenuItem(tr("Restaurar Layout Padrão", "Reset Layout"))) {
                apply_layout_defaults();
                apply_layout_visibility_to_imgui();
                ImGui::LoadIniSettingsFromDisk(m_layoutSettingsPath.c_str());
                ImGui::DockBuilderRemoveNode(ImGui::GetID("VulkanEngineStudioDockspace"));
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Ajuda", "Help"))) {
            if (ImGui::MenuItem(tr("Como Usar (Guia)", "How to Use (Guide)"))) {
                m_wickedTools.showGuideWindow = !m_wickedTools.showGuideWindow;
            }
            if (ImGui::MenuItem(tr("Painel de Desenvolvimento", "Developer Panel"))) {
                m_wickedTools.showDevWindow = !m_wickedTools.showDevWindow;
            }
            if (ImGui::MenuItem(tr("Manual da Engine", "VulkanCraft Documentation"))) {
                // Open the docs folder in Explorer (best effort).
                std::string docsDir = "docs";
                if (std::filesystem::exists("docs")) {
                    ShellExecuteW(nullptr, L"open", L"docs", nullptr, nullptr, SW_SHOWNORMAL);
                } else if (std::filesystem::exists("../docs")) {
                    ShellExecuteW(nullptr, L"open", L"..\\docs", nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            if (ImGui::MenuItem(tr("Sobre a Engine", "About VulkanCraft Engine"))) {
                m_showAboutDialog = true;
            }
            ImGui::EndMenu();
        }

        // Sobre — modal simples.
        if (m_showAboutDialog) {
            ImGui::OpenPopup(tr("Sobre a Engine", "About"));
            m_showAboutDialog = false;
        }
        if (ImGui::BeginPopupModal(tr("Sobre a Engine", "About"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("VulkanCraft Engine 1.5.0");
            ImGui::Separator();
            ImGui::TextWrapped("%s", tr(
                "Engine de jogos em Vulkan 1.3 (renderer, cena, ECS, física, "
                "assets, voxel, navegação, áudio, scripting). Frontend do editor "
                "inspirado/portado do Wicked Engine (MIT) — ver "
                "src/editor/frontend/PORTS.md.",
                "Vulkan game engine on Vulkan 1.3 (renderer, scene, ECS, physics, "
                "assets, voxel, navigation, audio, scripting). Editor frontend "
                "ported/inspired by Wicked Engine (MIT) — see "
                "src/editor/frontend/PORTS.md."));
            if (ImGui::Button(tr("Fechar", "Close"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Novo Jogo: confirm before discarding the current scene + name it.
        if (m_pendingNewSceneConfirm) {
            ImGui::OpenPopup(tr("Novo Jogo", "New Scene"));
            m_pendingNewSceneConfirm = false;
        }
        if (ImGui::BeginPopupModal(tr("Novo Jogo", "New Scene"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", tr("Criar uma nova cena? A cena atual será descartada.",
                                          "Create a new scene? The current scene will be discarded."));
            ImGui::Separator();
            ImGui::InputText(tr("Nome da Cena", "Scene Name"), m_newSceneName, sizeof(m_newSceneName));
            ImGui::Spacing();
            if (ImGui::Button(tr("Salvar e Criar", "Save & Create"), ImVec2(150, 0))) {
                save_current_scene();
                create_new_scene();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Criar Sem Salvar", "Create Without Saving"), ImVec2(180, 0))) {
                create_new_scene();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancelar", "Cancel"), ImVec2(110, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorApplication::draw_app_bar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float kBarHeight = 56.0f;

    // Position from viewport->Pos/Size: the menu bar occupies the first
    // frame height, the app bar sits right below it (no WorkPos double-offset).
    const float menuBarHeight = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, kBarHeight), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 10.0f));
    // No global WindowMinSize may inflate this fixed 56 px bar.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, UI::Colors::Surface);

    ImGui::Begin("##AppBar", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);

    // Search state is shared with the command palette drawn below the table.
    static char search[128]{};
    ImVec2 searchMin{ 0.0f, 0.0f };
    ImVec2 searchMax{ 0.0f, 0.0f };

    // 3-column responsive shell: Left (logo + actions) | Center (PLAY) |
    // Right (search + config). The stretch columns absorb the window width so
    // nothing is hard-positioned by fixed viewport math, and the table clips
    // cell content instead of letting any element overflow the bar.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 0.0f));
    if (ImGui::BeginTable("##AppBarLayout", 3,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Center", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        // Left: logo + product name + New Scene / Import / Save.
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(UI::Colors::Accent, "%s", ICON_FA_CUBES);
        ImGui::SameLine();
        ImGui::TextUnformatted("VulkanCraft");
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_PLUS "  Nova Cena", ICON_FA_PLUS "  New Scene"), ImVec2(122, 36))) {
            m_pendingNewSceneConfirm = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_FILE_IMPORT "  Importar", ICON_FA_FILE_IMPORT "  Import"), ImVec2(108, 36))) {
            std::string path;
            if (pick_file_dialog(path, L"Assets (*.*)\0*.*\0", L"Importar Asset", nullptr)) {
                if (m_assetPipeline) {
                    const std::filesystem::path cookedRoot =
                        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
                    const ImportResult result = m_assetPipeline->import({ path, cookedRoot, 1 });
                    if (result) {
                        std::cout << "[Editor] Asset importado: " << result.asset.sourcePath.filename().string() << std::endl;
                    } else {
                        std::cout << "[Editor] Falha ao importar: " << result.error << std::endl;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_FLOPPY_DISK "  Salvar", ICON_FA_FLOPPY_DISK "  Save"), ImVec2(96, 36))) {
            save_current_scene();
        }
        ImGui::SameLine();
        // [Build] — the fundamental action lives in the app bar, not buried in
        // Arquivo > Exportar.
        if (ImGui::Button(tr(ICON_FA_HAMMER "  Build", ICON_FA_HAMMER "  Build"), ImVec2(96, 36))) {
            run_game_build();
        }

        // Center: the single play button (green to start/resume, amber to
        // pause; right-click while playing = PARAR), centered in its column.
        ImGui::TableSetColumnIndex(1);
        const float btnWidth = 150.0f;
        const float btnHeight = 36.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             ImMax(0.0f, (ImGui::GetContentRegionAvail().x - btnWidth) * 0.5f));

        const PlayState state = m_playMode.get_state();
        const bool inPlay = state != PlayState::Edit;
        const bool paused = state == PlayState::Pause;
        const std::string playLabel = std::string(" " ICON_FA_PLAY "  ") + tr("TESTAR JOGO", "PLAY");
        const std::string resumeLabel = std::string(" " ICON_FA_PLAY "  ") + tr("CONTINUAR", "RESUME");
        const std::string pauseLabel = std::string(" " ICON_FA_PAUSE "  ") + tr("PAUSAR", "PAUSE");

        if (inPlay && !paused) {
            ImGui::PushStyleColor(ImGuiCol_Button, UI::Colors::Warning);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.72f, 0.25f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, UI::Colors::Success);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.78f, 0.45f, 1.0f));
        }
        const char* playBtnLabel = paused ? resumeLabel.c_str() : (inPlay ? pauseLabel.c_str() : playLabel.c_str());
        if (ImGui::Button(playBtnLabel, ImVec2(btnWidth, btnHeight))) {
            if (!inPlay) {
                m_playMode.start_play(m_editorScene.get());
                setup_play_runtime();
            } else {
                m_playMode.pause_play();
            }
        }
        // IsItemHovered() is false while the tooltip popup is open, so use the
        // raw rect test for the right-click stop.
        if (inPlay && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
            ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
            teardown_play_runtime();
            m_playMode.stop_play();
            m_playMode.set_editor_scene(m_editorScene.get());
            m_selectedEntity = Entity();
            m_editorGui.select_entity(m_selectedEntity);
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", inPlay
                ? tr("Clique: PAUSAR/CONTINUAR • Clique direito: PARAR o jogo",
                     "Click: PAUSE/RESUME • Right-click: STOP the game")
                : tr("Inicia o jogo interno (Play In Editor) — física, scripts, partículas e armas rodam no viewport",
                     "Starts the in-engine game (Play In Editor) — physics, scripts, particles and weapons run in the viewport"));
        }
        // PASSO: single-frame step while paused (was Control-API-only).
        if (paused) {
            ImGui::SameLine();
            if (ImGui::Button(tr(" PASSO ", " STEP "), ImVec2(0, btnHeight))) {
                m_stepRequested = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("Avança um único frame do jogo pausado (equivalente ao comando 'step' da Control API)", "Advances the paused game a single frame (same as the 'step' Control API command)"));
            }
        }

        // Right: search (stretches) + help + settings. The search box doubles
        // as a real command palette (Ctrl+K focuses it; see below).
        ImGui::TableSetColumnIndex(2);
        const float iconArea = 2.0f * 26.0f + 2.0f * ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImMax(80.0f, ImGui::GetContentRegionAvail().x - iconArea));
        if (m_focusGlobalSearch) {
            ImGui::SetKeyboardFocusHere();
            m_focusGlobalSearch = false;
        }
        ImGui::InputTextWithHint("##GlobalSearch", ICON_FA_MAGNIFYING_GLASS "  Buscar (Ctrl+K)", search, sizeof(search));
        const ImVec2 searchMin = ImGui::GetItemRectMin();
        const ImVec2 searchMax = ImGui::GetItemRectMax();
        ImGui::SameLine();
        if (UI::iconButton(ICON_FA_CIRCLE_QUESTION, tr("Ajuda", "Help"))) {
            m_showAboutDialog = true;
        }
        ImGui::SameLine();
        if (UI::iconButton(ICON_FA_GEAR, tr("Configurações", "Settings"))) {
            m_wickedTools.showGeneralWindow = !m_wickedTools.showGeneralWindow;
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    // Command palette: typing in the search box filters real commands, Enter
    // runs the first match, Esc closes. Drawn outside the table (a floating
    // window anchored below the search field).
    if (search[0] != '\0') {
        const float paletteW = ImMax(260.0f, searchMax.x - searchMin.x);
        ImGui::SetNextWindowPos(ImVec2(searchMin.x, searchMax.y + 6.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(paletteW, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::Begin("##CommandPalette", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PopStyleVar();

        std::string query = search;
        std::transform(query.begin(), query.end(), query.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        struct PaletteCmd { const char* label; std::function<void()> run; };
        const std::vector<PaletteCmd> cmds = {
            { tr("Novo Jogo...", "New Scene..."), [this]() { m_pendingNewSceneConfirm = true; } },
            { tr("Abrir Cena...", "Open Scene..."), [this]() {
                std::string p;
                if (pick_file_dialog(p, L"Cenas VulkanCraft (*.scene)\0*.scene\0Todos (*.*)\0*.*\0", L"Abrir Cena", L"scene")) load_scene_file(p);
            } },
            { tr("Salvar Cena", "Save Scene"), [this]() { save_current_scene(); } },
            { tr("Salvar Como...", "Save Scene As..."), [this]() { save_scene_as(); } },
            { tr("Build / Exportar Jogo (.exe)", "Build / Export Executable"), [this]() { run_game_build(); } },
            { tr("Importar Asset...", "Import Asset..."), [this]() {
                std::string path;
                if (pick_file_dialog(path, L"Assets (*.*)\0*.*\0", L"Importar Asset", nullptr)) {
                    if (m_assetPipeline) {
                        const std::filesystem::path cookedRoot =
                            std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
                        const ImportResult result = m_assetPipeline->import({ path, cookedRoot, 1 });
                        if (!result) std::cerr << "[Editor] " << result.error << std::endl;
                    }
                }
            } },
            { tr("Adicionar Cubo", "Add Cube"), [this]() {
                if (m_editorScene) {
                    Entity e = m_editorScene->create_entity(tr("Cubo 3D", "Cube"));
                    m_editorScene->meshRendererComponents[e.get_id()] = MeshRendererComponent{};
                    m_selectedEntity = e;
                }
            } },
            { tr("Adicionar Objeto Vazio", "Add Empty Object"), [this]() {
                if (m_editorScene) m_selectedEntity = m_editorScene->create_entity(tr("Novo Objeto", "New Entity"));
            } },
            { tr("Adicionar Luz do Sol", "Add Directional Light"), [this]() {
                if (m_editorScene) {
                    Entity e = m_editorScene->create_entity(tr("Luz do Sol", "Directional Light"));
                    m_editorScene->lightComponents[e.get_id()] = LightComponent{};
                    m_selectedEntity = e;
                }
            } },
            { tr("Testar Jogo / Parar", "Play / Stop"), [this]() {
                if (m_playMode.get_state() == PlayState::Edit) {
                    m_playMode.start_play(m_editorScene.get());
                    setup_play_runtime();
                } else {
                    teardown_play_runtime();
                    m_playMode.stop_play();
                    m_playMode.set_editor_scene(m_editorScene.get());
                    m_selectedEntity = Entity();
                    m_editorGui.select_entity(m_selectedEntity);
                }
            } },
            { tr("Abrir Guia de Uso", "Open How-to-Use Guide"), [this]() { m_wickedTools.showGuideWindow = true; } },
            { tr("Abrir Painel de Desenvolvimento", "Open Developer Panel"), [this]() { m_wickedTools.showDevWindow = true; } },
            { tr("Desfazer", "Undo"), [this]() { m_undo.undo(); mark_scene_dirty(); } },
            { tr("Refazer", "Redo"), [this]() { m_undo.redo(); mark_scene_dirty(); } },
            { tr("Alternar Viewport", "Toggle Viewport"), [this]() { m_showViewport = !m_showViewport; } },
            { tr("Alternar Cena", "Toggle Scene"), [this]() { m_showHierarchy = !m_showHierarchy; } },
            { tr("Alternar Inspector", "Toggle Inspector"), [this]() { m_showInspector = !m_showInspector; } },
            { tr("Alternar Assets", "Toggle Assets"), [this]() { m_showContentBrowser = !m_showContentBrowser; } },
            { tr("Alternar Console", "Toggle Console"), [this]() { m_showConsole = !m_showConsole; } },
            { tr("Alternar Grid", "Toggle Grid"), [this]() { m_showGrid = !m_showGrid; } },
            { tr("Alternar Gizmos", "Toggle Gizmos"), [this]() { m_showGizmos = !m_showGizmos; } },
            { tr("Alternar Colliders", "Toggle Colliders"), [this]() { m_showColliders = !m_showColliders; } },
            { tr("Idioma: PT / EN", "Language: PT / EN"), [this]() {
                m_currentLanguage = (m_currentLanguage == EngineLanguage::PT_BR) ? EngineLanguage::EN_US : EngineLanguage::PT_BR;
            } },
            { tr("Configurações", "Settings"), [this]() { m_wickedTools.showGeneralWindow = !m_wickedTools.showGeneralWindow; } },
            { tr("Sobre a Engine", "About"), [this]() { m_showAboutDialog = true; } },
        };

        int shown = 0;
        PaletteCmd firstMatch{ nullptr, nullptr };
        for (const auto& c : cmds) {
            std::string label = c.label;
            std::transform(label.begin(), label.end(), label.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (label.find(query) == std::string::npos) continue;
            if (!firstMatch.run) firstMatch = c;
            if (shown < 8) {
                if (ImGui::Selectable(c.label)) {
                    c.run();
                    search[0] = '\0';
                }
                ++shown;
            }
        }
        if (shown == 0) {
            ImGui::TextDisabled("%s", tr("Nenhum comando encontrado", "No matching commands"));
        }
        if (firstMatch.run && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
            firstMatch.run();
            search[0] = '\0';
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            search[0] = '\0';
        }

        ImGui::End();
    }

    ImGui::End();
}

void EditorApplication::draw_hierarchy_panel() {
    // Local minimum only (see draw_app_bar note about the global style).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(240.0f, 180.0f));
    ImGui::Begin(tr("Cena", "Scene"));
    ImGui::PopStyleVar();

    // Search + Add row: a real-time filter and the full entity creation menu.
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 52.0f);
    ImGui::InputTextWithHint("##SceneSearch", ICON_FA_MAGNIFYING_GLASS "  Buscar na cena...", m_hierarchySearch, sizeof(m_hierarchySearch));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS, ImVec2(40, 0))) ImGui::OpenPopup("##AddEntityMenu");

    const auto createSel = [this](const std::string& name) -> Entity {
        if (!m_editorScene) return Entity();
        Entity e = m_editorScene->create_entity(name);
        m_selectedEntity = e;
        mark_scene_dirty();
        return e;
    };
    if (ImGui::BeginPopup("##AddEntityMenu")) {
        ImGui::TextDisabled("%s", tr("BÁSICO", "BASIC"));
        if (ImGui::MenuItem(tr("Objeto Vazio", "Empty Object"))) createSel(tr("Novo Objeto", "New Entity"));
        if (ImGui::MenuItem(tr("Cubo 3D", "Cube"))) {
            Entity e = createSel(tr("Cubo 3D", "Cube"));
            if (e.is_valid()) m_editorScene->meshRendererComponents[e.get_id()] = MeshRendererComponent{};
        }
        if (ImGui::MenuItem(tr("Câmera", "Camera"))) {
            Entity e = createSel(tr("Câmera", "Camera"));
            if (e.is_valid()) m_editorScene->cameraComponents[e.get_id()] = CameraComponent{};
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("ILUMINAÇÃO", "LIGHTING"));
        if (ImGui::MenuItem(tr("Luz do Sol", "Directional Light"))) {
            Entity e = createSel(tr("Luz do Sol", "Directional Light"));
            if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{};
        }
        if (ImGui::MenuItem(tr("Luz de Lâmpada", "Point Light"))) {
            Entity e = createSel(tr("Luz de Lâmpada", "Point Light"));
            if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.8f, 0.4f), 5000.0f, 15.0f, true };
        }
        if (ImGui::MenuItem(tr("Luz Spot", "Spot Light"))) {
            Entity e = createSel(tr("Luz Spot", "Spot Light"));
            if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot };
        }
        if (ImGui::MenuItem(tr("Luz de Área", "Area Light"))) {
            Entity e = createSel(tr("Luz de Área", "Area Light"));
            if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area };
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("EFEITOS", "EFFECTS"));
        if (ImGui::MenuItem(tr("Emissor de Partículas", "Particle Emitter"))) {
            Entity e = createSel(tr("Emissor de Partículas", "Particle Emitter"));
            if (e.is_valid()) m_editorScene->particleEmitterComponents[e.get_id()] = ParticleEmitterComponent{};
        }
        if (ImGui::MenuItem(tr("Fonte de Áudio", "Audio Source"))) {
            Entity e = createSel(tr("Fonte de Áudio", "Audio Source"));
            if (e.is_valid()) m_editorScene->audioComponents[e.get_id()] = AudioComponent{};
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("FÍSICA / GAMEPLAY", "PHYSICS / GAMEPLAY"));
        if (ImGui::MenuItem(tr("Corpo Rígido", "Rigidbody Object"))) {
            Entity e = createSel(tr("Corpo Rígido", "Rigidbody Object"));
            if (e.is_valid()) m_editorScene->rigidbodyComponents[e.get_id()] = RigidbodyComponent{};
        }
        if (ImGui::MenuItem(tr("Veículo", "Vehicle"))) {
            Entity e = createSel(tr("Veículo", "Vehicle"));
            if (e.is_valid()) m_editorScene->vehicleComponents[e.get_id()] = VehicleComponent{};
        }
        if (ImGui::MenuItem(tr("Destrutível", "Destructible"))) {
            Entity e = createSel(tr("Destrutível", "Destructible"));
            if (e.is_valid()) m_editorScene->destructionComponents[e.get_id()] = DestructionComponent{};
        }
        if (ImGui::MenuItem(tr("Agente de Navegação", "Navigation Agent"))) {
            Entity e = createSel(tr("Agente de Navegação", "Navigation Agent"));
            if (e.is_valid()) m_editorScene->navigationComponents[e.get_id()] = NavigationComponent{};
        }
        if (ImGui::MenuItem(tr("Missão", "Mission"))) {
            Entity e = createSel(tr("Missão", "Mission"));
            if (e.is_valid()) m_editorScene->missionComponents[e.get_id()] = MissionComponent{};
        }
        if (ImGui::MenuItem(tr("Diálogo", "Dialogue"))) {
            Entity e = createSel(tr("Diálogo", "Dialogue"));
            if (e.is_valid()) m_editorScene->dialogueComponents[e.get_id()] = DialogueComponent{};
        }
#if VC_ENABLE_VOXEL_PLUGIN
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("MUNDO", "WORLD"));
        if (ImGui::MenuItem(tr("Mundo de Blocos", "Voxel Volume"))) {
            Entity e = createSel(tr("Mundo de Blocos", "Voxel Volume"));
            if (e.is_valid()) m_editorScene->voxelVolumeComponents[e.get_id()] = VoxelVolumeComponent{};
        }
#endif
        ImGui::EndPopup();
    }
    ImGui::Separator();

    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    if (!scene) {
        ImGui::TextDisabled("%s", tr("Nenhuma cena aberta", "No scene open"));
        ImGui::End();
        return;
    }

    const std::string filter = m_hierarchySearch;
    static const char* kEntityDrag = "VC_ENTITY";

    // Recursive node renderer: real parent/child tree (roots first), with
    // drag-to-reparent (cycle-safe) and a delete context menu.
    std::function<void(UUID)> drawNode = [&](UUID id) {
        const Entity* ent = scene->find_entity_by_id_const(id);
        if (!ent) return;
        const std::vector<UUID> children = scene->get_children(id);
        const bool hasChildren = !children.empty();

        ImGuiTreeNodeFlags flags = ((m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id) ? ImGuiTreeNodeFlags_Selected : 0) |
                                   ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                   (hasChildren ? 0 : (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen));

        ImVec4 iconColor = ImVec4(0.60f, 0.60f, 0.68f, 1.0f);
        if (scene->cameraComponents.contains(id)) iconColor = ImVec4(0.37f, 0.64f, 0.98f, 1.0f);
        else if (scene->lightComponents.contains(id)) iconColor = ImVec4(0.98f, 0.75f, 0.14f, 1.0f);
        else if (scene->voxelVolumeComponents.contains(id)) iconColor = ImVec4(0.20f, 0.82f, 0.60f, 1.0f);
        else if (scene->meshRendererComponents.contains(id)) iconColor = ImVec4(0.45f, 0.55f, 0.85f, 1.0f);
        else if (scene->particleEmitterComponents.contains(id)) iconColor = ImVec4(0.95f, 0.45f, 0.25f, 1.0f);

        ImGui::TextColored(iconColor, "%s", entityIcon(scene, id));
        ImGui::SameLine();

        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(id.get_high() ^ id.get_low()),
                                            flags, "%s", ent->get_name().c_str());
        if (ImGui::IsItemClicked()) {
            m_selectedEntity = *ent;
        }

        // Drag source: pick this entity up to reparent it.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            ImGui::SetDragDropPayload(kEntityDrag, &id, sizeof(UUID));
            ImGui::TextUnformatted(ent->get_name().c_str());
            ImGui::EndDragDropSource();
        }
        // Drop target: drop another entity here to make it a child.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDrag)) {
                UUID dragged;
                std::memcpy(&dragged, payload->Data, sizeof(UUID));
                if (dragged != id) {
                    scene->set_parent(dragged, id); // cycle-safe
                    mark_scene_dirty();
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(tr("Deletar Objeto", "Delete Entity"))) {
                scene->destroy_entity(id);
                if (m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id) m_selectedEntity = Entity();
                mark_scene_dirty();
            }
            ImGui::EndPopup();
        }

        if (hasChildren && open) {
            for (const UUID& child : children) drawNode(child);
            ImGui::TreePop();
        }
    };

    if (filter.empty()) {
        // Real hierarchy: roots first, then their children recursively.
        for (const auto& [id, entity] : scene->get_entities()) {
            (void)entity;
            if (!scene->get_parent(id).is_valid()) drawNode(id);
        }
    } else {
        // Search mode: flat list of matches (a parent may not match the query).
        for (const auto& [id, entity] : scene->get_entities()) {
            if (entity.get_name().find(filter) != std::string::npos) drawNode(id);
        }
    }

    ImGui::End();
}

void EditorApplication::draw_inspector_panel() {
    // Local minimum only (see draw_app_bar note about the global style).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(300.0f, 200.0f));
    ImGui::Begin(tr("Inspector", "Inspector"));
    ImGui::PopStyleVar();

    if (!m_selectedEntity.is_valid()) {
        ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected"));
        ImGui::End();
        return;
    }

    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    if (!scene) {
        ImGui::End();
        return;
    }

    UUID id = m_selectedEntity.get_id();

    // Entity header: name + advanced-mode toggle (Forge design). The UUID and
    // technical fields are hidden unless advanced mode is on.
    char nameBuf[256];
    strncpy(nameBuf, m_selectedEntity.get_name().c_str(), sizeof(nameBuf));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40.0f);
    if (ImGui::InputText("##EntityName", nameBuf, sizeof(nameBuf))) {
        m_selectedEntity.set_name(nameBuf);
    }
    ImGui::SameLine();
    UI::toggle("##AdvancedToggle", &m_advancedInspector,
               tr("Mostrar propriedades avançadas", "Show advanced properties"));
    if (m_advancedInspector) {
        ImGui::TextDisabled("Código Único: %s", id.to_string().c_str());
    }
    ImGui::Separator();

    // Transform Component — card with collapse + vec3 rows.
    if (scene->transformComponents.contains(id)) {
        if (UI::sectionHeader(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, tr("Transform", "Transform"))) {
            auto& t = scene->transformComponents[id];
            UI::vec3Property(tr("Posição", "Position"), &t.position.x, 0.1f);
            UI::vec3Property(tr("Rotação", "Rotation"), &t.rotation.x, 1.0f);
            UI::vec3Property(tr("Escala", "Scale"), &t.scale.x, 0.1f);
            ImGui::Spacing();
        }
    }

    // Semantic sections (Inspector): components are grouped by role —
    // Appearance / Physics / Gameplay / Effects & World. Each group header is
    // emitted once, before the first component of that group that exists.
    bool inspectorGroupEmitted[5] = { false, false, false, false, false };
    const auto beginInspectorGroup = [&](int group, const char* title) {
        if (inspectorGroupEmitted[group]) return;
        inspectorGroupEmitted[group] = true;
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Colors::Accent);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Separator();
    };

    // Mesh Renderer Component
    if (scene->meshRendererComponents.contains(id)) {
        beginInspectorGroup(0, tr("APARÊNCIA", "APPEARANCE"));
        UI::sectionHeader(ICON_FA_CUBE, tr("Malha", "Mesh Renderer"));
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

    // Rigidbody Component
    if (scene->rigidbodyComponents.contains(id)) {
        beginInspectorGroup(1, tr("FÍSICA", "PHYSICS"));
        UI::sectionHeader(ICON_FA_WEIGHT_HANGING, tr("Física", "Rigidbody"));
        auto& r = scene->rigidbodyComponents[id];
        ImGui::DragFloat(tr("Peso (kg)", "Mass (kg)"), &r.mass, 0.5f, 0.01f, 10000.0f);
        ImGui::SliderFloat(tr("Deslize (Fricção)", "Friction"), &r.friction, 0.0f, 1.0f);
        ImGui::SliderFloat(tr("Quique (Elasticidade)", "Restitution"), &r.restitution, 0.0f, 1.0f);
        ImGui::Checkbox(tr("Física Fixa (Sem Mover)", "Is Kinematic"), &r.isKinematic);
        ImGui::Checkbox(tr("Ativar Gravidade", "Use Gravity"), &r.useGravity);
        ImGui::Spacing();
    }

    // Destruction Component (a destructible of chunkCount boxes; weapon hits
    // within damageRadius detach chunks in play).
    if (scene->destructionComponents.contains(id)) {
        beginInspectorGroup(1, tr("FÍSICA", "PHYSICS"));
        UI::sectionHeader(ICON_FA_EXPLOSION, tr("Destrutível", "Destruction"));
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

    // Weapon Component (authored in the Weapon panel; the play world fires it).
    if (scene->weaponComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_GUN, tr("Arma", "Weapon"));
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

    // Vehicle Component (authored in the Vehicle panel; the play world builds
    // a chassis body + four wheels and drives it with a VehicleRuntime).
    if (scene->vehicleComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_CAR, tr("Veículo", "Vehicle"));
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
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_USER, tr("Ragdoll", "Ragdoll"));
        auto& rg = scene->ragdollComponents[id];
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &rg.enabled);
        ImGui::SliderFloat(tr("Blend da física", "Physics Blend"), &rg.blendWeight, 0.0f, 1.0f);
        ImGui::Checkbox(tr("Da esqueleto (skin)", "From skeleton (skin)"), &rg.fromSkeleton);
        ImGui::DragFloat(tr("Massa por osso", "Mass per bone"), &rg.massPerBone, 0.1f, 0.1f, 100.0f);
        ImGui::DragFloat3(tr("Deslocamento de spawn", "Spawn Offset"), &rg.spawnOffset.x, 0.1f);
        if (ImGui::Button(tr("Remover Ragdoll", "Remove Ragdoll"))) scene->ragdollComponents.erase(id);
        ImGui::Spacing();
    }

    // Animation Component (authored in the Animation editor; the play world
    // samples the entry state's clip onto the bone entities under this one).
    if (scene->animationComponents.contains(id)) {
        beginInspectorGroup(4, tr("ANIMAÇÃO", "ANIMATION"));
        UI::sectionHeader(ICON_FA_FILM, tr("Animação", "Animation"));
        auto& an = scene->animationComponents[id];
        ImGui::Checkbox(tr("Tocando", "Playing"), &an.playing);
        char entryBuf[64];
        std::snprintf(entryBuf, sizeof(entryBuf), "%s", an.entryState.c_str());
        if (ImGui::InputText(tr("Estado de entrada", "Entry State"), entryBuf, sizeof(entryBuf))) an.entryState = entryBuf;
        ImGui::TextDisabled("%zu %s", an.states.size(), tr("estados", "states"));
        for (const auto& s : an.states) {
            ImGui::BulletText("%s (clip %s, x%.2f)", s.id.c_str(), s.clip.to_string().c_str(), s.speed);
        }
        if (ImGui::Button(tr("Remover Animação", "Remove Animation"))) scene->animationComponents.erase(id);
        ImGui::Spacing();
    }

    // Timeline Component (authored in the Timeline editor; the play world
    // animates the entity's transform from Property tracks).
    if (scene->timelineComponents.contains(id)) {
        beginInspectorGroup(4, tr("ANIMAÇÃO", "ANIMATION"));
        UI::sectionHeader(ICON_FA_CLOCK, tr("Timeline", "Timeline"));
        auto& tl = scene->timelineComponents[id];
        ImGui::DragFloat(tr("Duração", "Duration"), &tl.duration, 0.1f, 0.01f, 10000.0f);
        ImGui::SliderFloat(tr("Playhead", "Playhead"), &tl.playhead, 0.0f, std::max(tl.duration, 0.01f));
        ImGui::Checkbox(tr("Loop", "Loop"), &tl.loop);
        ImGui::TextDisabled("%zu %s", tl.tracks.size(), tr("trilhas", "tracks"));
        for (const auto& t : tl.tracks) ImGui::BulletText("%s (%zu %s)", t.name.c_str(), t.keys.size(), tr("chaves", "keys"));
        if (ImGui::Button(tr("Remover Timeline", "Remove Timeline"))) scene->timelineComponents.erase(id);
        ImGui::Spacing();
    }

    // IK Component (authored in the IK editor; the play world bends the
    // chain root -> mid -> end so the end entity reaches the target entity).
    if (scene->ikComponents.contains(id)) {
        beginInspectorGroup(4, tr("ANIMAÇÃO", "ANIMATION"));
        UI::sectionHeader(ICON_FA_BONE, tr("IK", "IK"));
        auto& ik = scene->ikComponents[id];
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &ik.enabled);
        ImGui::SliderFloat(tr("Peso", "Weight"), &ik.weight, 0.0f, 1.0f);
        ImGui::DragInt(tr("Iterações", "Iterations"), &ik.iterations, 1, 1, 64);
        ImGui::TextDisabled("root=%s mid=%s end=%s target=%s", ik.rootEntity.to_string().c_str(),
                            ik.midEntity.to_string().c_str(), ik.endEntity.to_string().c_str(),
                            ik.targetEntity.to_string().c_str());
        if (ImGui::Button(tr("Remover IK", "Remove IK"))) scene->ikComponents.erase(id);
        ImGui::Spacing();
    }

    // Retarget Component (authored in the Retarget editor; the play world
    // copies mapped source-bone transforms onto the target-bone entities).
    if (scene->retargetComponents.contains(id)) {
        beginInspectorGroup(4, tr("ANIMAÇÃO", "ANIMATION"));
        UI::sectionHeader(ICON_FA_SHUFFLE, tr("Retarget", "Retarget"));
        auto& rt = scene->retargetComponents[id];
        ImGui::Checkbox(tr("Preservar root motion", "Preserve Root Motion"), &rt.preserveRootMotion);
        ImGui::TextDisabled("%zu %s", rt.mapping.size(), tr("mapeamentos", "mappings"));
        for (const auto& m : rt.mapping) ImGui::BulletText("%s -> %s", m.sourceBone.c_str(), m.targetBone.c_str());
        if (ImGui::Button(tr("Remover Retarget", "Remove Retarget"))) scene->retargetComponents.erase(id);
        ImGui::Spacing();
    }

    // Mission Component (the play world registers a Mission that the
    // completeEvent — a script EmitEvent — finishes).
    if (scene->missionComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_FLAG, tr("Missão", "Mission"));
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
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_COMMENT, tr("Diálogo", "Dialogue"));
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

    // Navigation Component (a baked grid + an agent toward the camera).
    if (scene->navigationComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_LOCATION_CROSSHAIRS, tr("Navegação", "Navigation"));
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
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        UI::sectionHeader(ICON_FA_VOLUME_HIGH, tr("Áudio", "Audio"));
        auto& au = scene->audioComponents[id];
        char clipBuf[256]; std::snprintf(clipBuf, sizeof(clipBuf), "%s", au.clipPath.c_str());
        if (ImGui::InputText(".ogg", clipBuf, sizeof(clipBuf))) au.clipPath = clipBuf;
        ImGui::DragFloat(tr("Volume", "Volume"), &au.volume, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat(tr("Pitch", "Pitch"), &au.pitch, 0.01f, 0.1f, 4.0f);
        ImGui::Checkbox(tr("Espacial", "Spatial"), &au.spatial);
        ImGui::SameLine();
        ImGui::Checkbox(tr("Em loop", "Looping"), &au.looping);
        ImGui::Checkbox(tr("Tocar ao iniciar", "Play On Start"), &au.playOnStart);        if (ImGui::Button(tr("Remover Áudio", "Remove Audio"))) scene->audioComponents.erase(id);
        ImGui::Spacing();
    }

    // Light Component
    if (scene->lightComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        UI::sectionHeader(ICON_FA_SUN, tr("Luz", "Light"));
        auto& l = scene->lightComponents[id];
        ImGui::ColorEdit3(tr("Cor da Luz", "Light Color"), &l.color.r);
        ImGui::DragFloat(tr("Brilho (Intensidade)", "Intensity"), &l.intensity, 100.0f, 0.0f, 100000.0f);
        ImGui::DragFloat(tr("Alcance da Luz", "Range"), &l.range, 0.5f, 0.1f, 1000.0f);
        ImGui::Checkbox(tr("Projetar Sombras", "Cast Shadows"), &l.castShadows);
        ImGui::Spacing();
    }

    // Camera Component — near/far planes are advanced-only fields.
    if (scene->cameraComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        UI::sectionHeader(ICON_FA_CAMERA, tr("Câmera", "Camera"));
        auto& c = scene->cameraComponents[id];
        ImGui::SliderFloat(tr("Campo de Visão (FOV)", "Field of View (FOV)"), &c.fov, 10.0f, 160.0f);
        if (m_advancedInspector) {
            ImGui::DragFloat(tr("Visão Próxima", "Near Plane"), &c.nearPlane, 0.01f, 0.001f, 10.0f);
            ImGui::DragFloat(tr("Visão Distante", "Far Plane"), &c.farPlane, 100.0f, 10.0f, 100000.0f);
        }
        ImGui::Checkbox(tr("Câmera Principal do Jogo", "Primary Camera"), &c.isPrimary);
        ImGui::Spacing();
    }

    // Particle Emitter Component (authored in the Particle panel; the play
    // world instantiates a ParticleSimulation emitter at the entity origin).
    if (scene->particleEmitterComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        UI::sectionHeader(ICON_FA_FIRE, tr("Partículas", "Particle Emitter"));
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

#if VC_ENABLE_VOXEL_PLUGIN

    // Voxel Volume Component
    if (scene->voxelVolumeComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
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
        static char compSearch[64]{};
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputTextWithHint("##CompSearch", tr("Buscar componentes...", "Search components..."), compSearch, sizeof(compSearch));
        ImGui::Separator();

        const std::string cq = compSearch;
        const auto match = [&](const char* label) {
            if (cq.empty()) return true;
            std::string l = label;
            std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            std::string q = cq;
            std::transform(q.begin(), q.end(), q.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return l.find(q) != std::string::npos;
        };
        const auto section = [&](const char* title) {
            if (cq.empty()) ImGui::TextDisabled("%s", title);
        };

        section(tr("COMUM", "COMMON"));
        if (match(tr("Iluminação e Luz", "Light"))) { if (ImGui::MenuItem(tr("Iluminação e Luz", "Light Component"))) scene->lightComponents[id] = LightComponent{}; }
        if (match(tr("Câmera de Visão", "Camera"))) { if (ImGui::MenuItem(tr("Câmera de Visão", "Camera Component"))) scene->cameraComponents[id] = CameraComponent{}; }
        if (match(tr("Modelo 3D (Mesh)", "Mesh Renderer"))) { if (ImGui::MenuItem(tr("Modelo 3D (Mesh)", "Mesh Renderer"))) scene->meshRendererComponents[id] = MeshRendererComponent{}; }
        if (match(tr("Material", "Material"))) { if (ImGui::MenuItem(tr("Material", "Material Component"))) scene->materialComponents[id] = MaterialComponent{}; }
        ImGui::Separator();
        section(tr("FÍSICA / GAMEPLAY", "PHYSICS / GAMEPLAY"));
        if (match(tr("Física e Gravidade", "Rigidbody"))) { if (ImGui::MenuItem(tr("Física e Gravidade", "Rigidbody Component"))) scene->rigidbodyComponents[id] = RigidbodyComponent{}; }
        if (match(tr("Arma (Hitscan)", "Weapon"))) { if (ImGui::MenuItem(tr("Arma (Hitscan)", "Weapon Component"))) scene->weaponComponents[id] = WeaponComponent{}; }
        if (match(tr("Veículo", "Vehicle"))) { if (ImGui::MenuItem(tr("Veículo", "Vehicle Component"))) scene->vehicleComponents[id] = VehicleComponent{}; }
        if (match(tr("Ragdoll", "Ragdoll"))) { if (ImGui::MenuItem(tr("Ragdoll", "Ragdoll Component"))) scene->ragdollComponents[id] = RagdollComponent{}; }
        if (match(tr("Destrutível", "Destructible"))) { if (ImGui::MenuItem(tr("Destrutível", "Destruction Component"))) scene->destructionComponents[id] = DestructionComponent{}; }
        if (match(tr("Navegação", "Navigation"))) { if (ImGui::MenuItem(tr("Navegação", "Navigation Component"))) scene->navigationComponents[id] = NavigationComponent{}; }
        ImGui::Separator();
        section(tr("EFEITOS / NARRATIVA", "EFFECTS / NARRATIVE"));
        if (match(tr("Emissor de Partículas", "Particle"))) { if (ImGui::MenuItem(tr("Emissor de Partículas", "Particle Emitter Component"))) scene->particleEmitterComponents[id] = ParticleEmitterComponent{}; }
        if (match(tr("Fonte de Áudio", "Audio"))) { if (ImGui::MenuItem(tr("Fonte de Áudio", "Audio Component"))) scene->audioComponents[id] = AudioComponent{}; }
        if (match(tr("Missão", "Mission"))) { if (ImGui::MenuItem(tr("Missão", "Mission Component"))) scene->missionComponents[id] = MissionComponent{}; }
        if (match(tr("Diálogo", "Dialogue"))) { if (ImGui::MenuItem(tr("Diálogo", "Dialogue Component"))) scene->dialogueComponents[id] = DialogueComponent{}; }
        ImGui::Separator();
        section(tr("ANIMAÇÃO", "ANIMATION"));
        if (match(tr("Máquina de Estados (Animação)", "Animation State Machine"))) { if (ImGui::MenuItem(tr("Máquina de Estados (Animação)", "Animation Component"))) scene->animationComponents[id] = AnimationComponent{}; }
        if (match(tr("Timeline", "Timeline"))) { if (ImGui::MenuItem(tr("Timeline", "Timeline Component"))) scene->timelineComponents[id] = TimelineComponent{}; }
        if (match(tr("IK (Cadeia de Ossos)", "IK Chain"))) { if (ImGui::MenuItem(tr("IK (Cadeia de Ossos)", "IK Component"))) scene->ikComponents[id] = IKComponent{}; }
        if (match(tr("Retargeting de Esqueleto", "Retarget"))) { if (ImGui::MenuItem(tr("Retargeting de Esqueleto", "Retarget Component"))) scene->retargetComponents[id] = RetargetComponent{}; }
#if VC_ENABLE_VOXEL_PLUGIN
        ImGui::Separator();
        section(tr("MUNDO", "WORLD"));
        if (match(tr("Mundo de Blocos", "Voxel"))) { if (ImGui::MenuItem(tr("Mundo de Blocos", "Voxel Terrain Volume"))) scene->voxelVolumeComponents[id] = VoxelVolumeComponent{}; }
#endif
        ImGui::EndPopup();
    }

    // Inspector edits (drag floats, color pickers, combos, the name field,
    // Add-Component popup): any mouse release inside the panel ends an edit —
    // mark the scene dirty so autosave persists it. A spurious save of an
    // unchanged scene is harmless (debounce caps the frequency).
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        mark_scene_dirty();
    }

    ImGui::End();
}

void EditorApplication::draw_viewport_panel() {
    // No scrollbar: the viewport must fill the allowed area 1:1. The offscreen
    // target is sized to the image area (content minus the header) so the
    // rendered image exactly matches the panel — nothing overflows.
    ImGui::Begin(tr("Viewport", "Viewport"), nullptr,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 panelSize = ImGui::GetContentRegionAvail();

    // Viewport toolbar: gizmo mode (Move/Rotate/Scale), the camera label and
    // the live play-mode badge. Responsive: secondary info drops first as the
    // panel narrows (panel size → camera label); the gizmo buttons and the
    // play badge always remain (badge = status, buttons = actions).
    const float toolbarWidth = panelSize.x;
    // Select / Move / Rotate / Scale (Q/W/E/R). Select hides the gizmo and
    // only picks entities.
    if (UI::iconButton(ICON_FA_HAND_POINTER, tr("Selecionar (Q)", "Select (Q)"),
                       m_gizmoMode == GizmoMode::Select)) m_gizmoMode = GizmoMode::Select;
    ImGui::SameLine();
    if (UI::iconButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, tr("Mover (W)", "Move (W)"),
                       m_gizmoMode == GizmoMode::Translate)) m_gizmoMode = GizmoMode::Translate;
    ImGui::SameLine();
    if (UI::iconButton(ICON_FA_ROTATE, tr("Rotar (E)", "Rotate (E)"),
                       m_gizmoMode == GizmoMode::Rotate)) m_gizmoMode = GizmoMode::Rotate;
    ImGui::SameLine();
    if (UI::iconButton(ICON_FA_UP_DOWN_LEFT_RIGHT, tr("Escalar (R)", "Scale (R)"),
                       m_gizmoMode == GizmoMode::Scale)) m_gizmoMode = GizmoMode::Scale;
    if (toolbarWidth > 420.0f) {
        // World/Local gizmo space: Local rotates the drag axes with the entity.
        ImGui::SameLine();
        if (UI::iconButton(m_gizmoLocal ? ICON_FA_CUBE : ICON_FA_GLOBE,
                           m_gizmoLocal ? tr("Local", "Local") : tr("Mundo", "World"),
                           m_gizmoLocal)) {
            m_gizmoLocal = !m_gizmoLocal;
        }
        // Snap step used by Ctrl-drag (translate); 0 disables snapping.
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        const char* snapLabels[] = { "Snap 0", "Snap 0.1", "Snap 0.5", "Snap 1", "Snap 2", "Snap 5" };
        const float snapValues[] = { 0.0f, 0.1f, 0.5f, 1.0f, 2.0f, 5.0f };
        int snapIdx = 2;
        for (int i = 0; i < IM_ARRAYSIZE(snapValues); ++i) {
            if (std::abs(m_snapTranslate - snapValues[i]) < 1e-4f) snapIdx = i;
        }
        if (ImGui::BeginCombo("##SnapStep", snapLabels[snapIdx], ImGuiComboFlags_NoArrowButton)) {
            for (int i = 0; i < IM_ARRAYSIZE(snapValues); ++i) {
                if (ImGui::Selectable(snapLabels[i], i == snapIdx)) m_snapTranslate = snapValues[i];
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Passo de snap ao segurar Ctrl ao arrastar o gizmo", "Snap step when holding Ctrl while dragging the gizmo"));
        }
    }
    if (toolbarWidth > 350.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Perspectiva", "Perspective"));
    }
    // Live play-mode state in the header: the scene looks identical while the
    // in-engine game runs, so the mode must be explicit.
    const PlayState headerState = m_playMode.get_state();
    if (headerState != PlayState::Edit) {
        const bool paused = headerState == PlayState::Pause;
        const std::string stateTag = std::string(paused ? ICON_FA_PAUSE
                                                        : (headerState == PlayState::Simulate ? ICON_FA_ARROWS_ROTATE : ICON_FA_PLAY)) +
            "  " + (paused ? tr("PAUSADO", "PAUSED")
                           : (headerState == PlayState::Simulate ? tr("SIMULANDO", "SIMULATING")
                                                                 : tr("JOGO EM EXECUÇÃO", "PLAYING")));
        ImGui::SameLine();
        ImGui::TextColored(paused ? ImVec4(0.96f, 0.62f, 0.04f, 1.0f) : ImVec4(0.30f, 0.90f, 0.60f, 1.0f),
                           "%s", stateTag.c_str());
    }
    if (toolbarWidth > 500.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  %dx%d", static_cast<int>(panelSize.x), static_cast<int>(panelSize.y));
    }
    // Overflow menu: display toggles that used to be hidden in Janelas.
    ImGui::SameLine();
    if (UI::iconButton(ICON_FA_ELLIPSIS_VERTICAL, tr("Opções do Viewport", "Viewport Options"))) {
        ImGui::OpenPopup("##ViewportOptions");
    }
    if (ImGui::BeginPopup("##ViewportOptions")) {
        ImGui::MenuItem(tr("Grid", "Grid"), nullptr, &m_showGrid);
        ImGui::MenuItem(tr("Gizmos", "Gizmos"), nullptr, &m_showGizmos);
        ImGui::MenuItem(tr("Colliders (wireframe)", "Collider Wireframes"), nullptr, &m_showColliders);
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Segure Ctrl ao arrastar o gizmo para usar snap", "Hold Ctrl while dragging the gizmo to snap"));
        ImGui::EndPopup();
    }
    ImGui::Separator();

    // The image area is what remains BELOW the header. Measure it AFTER the
    // header is drawn — the header is two lines + separator, so estimating it
    // from a single frame height overflowed and the mouse wheel scrolled the
    // panel (the NoScrollbar flag hides the bar but does NOT stop the wheel).
    const ImVec2 imageAvail = ImGui::GetContentRegionAvail();
    const float availW = std::max(1.0f, imageAvail.x);
    const float availH = std::max(1.0f, imageAvail.y);
    m_viewportPanelSize = imageAvail;

    m_viewportHovered = ImGui::IsWindowHovered();
    m_viewportFocused = ImGui::IsWindowFocused();

    if (m_offscreen.imguiTextureID == VK_NULL_HANDLE) {
        ImGui::TextDisabled("%s", tr("O viewport 3D será criado ao entrar no editor...", "3D viewport is being prepared..."));
        ImGui::End();
        return;
    }

    // Fit the offscreen texture into the remaining image area, preserving the
    // aspect ratio: fills 100% of the allowed space, centered, no overflow.
    const float texAspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    float dispW = availW;
    float dispH = dispW / texAspect;
    if (dispH > availH) {
        dispH = availH;
        dispW = dispH * texAspect;
    }
    const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    const ImVec2 dispPos(cursorPos.x + std::max(0.0f, (availW - dispW) * 0.5f), cursorPos.y);
    m_viewportImagePos = dispPos;
    m_viewportImageSize = ImVec2(dispW, dispH);
    m_viewportImageHovered = ImGui::IsMouseHoveringRect(dispPos, ImVec2(dispPos.x + dispW, dispPos.y + dispH));

    ImGui::SetCursorScreenPos(dispPos);
    // Vulkan images are top-down; flip V so the world appears upright.
    ImGui::Image(m_offscreen.imguiTextureID, ImVec2(dispW, dispH), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    // Play-mode feedback overlay: while the in-engine game runs, draw a colored
    // border + state badge over the viewport so the mode is unmistakable (the
    // rendered scene itself is identical to edit mode). When the play world has
    // nothing that animates, show a hint instead of silence — the most common
    // "o botão não fez nada" confusion is an empty scene.
    const PlayState playState = m_playMode.get_state();
    if (playState != PlayState::Edit) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool paused = playState == PlayState::Pause;
        const ImU32 accent = paused ? IM_COL32(245, 158, 11, 235) : IM_COL32(16, 185, 129, 235);
        dl->AddRect(dispPos, ImVec2(dispPos.x + dispW, dispPos.y + dispH), accent, 0.0f, 0, 3.0f);

        const std::string badge = std::string(paused ? ICON_FA_PAUSE : ICON_FA_PLAY) + "  " +
            (paused ? tr("PAUSADO", "PAUSED") : tr("JOGO EM EXECUÇÃO", "PLAYING"));
        const ImVec2 badgeSize = ImGui::CalcTextSize(badge.c_str());
        const ImVec2 badgePos(dispPos.x + 10.0f, dispPos.y + 10.0f);
        dl->AddRectFilled(badgePos,
                          ImVec2(badgePos.x + badgeSize.x + 16.0f, badgePos.y + badgeSize.y + 8.0f),
                          IM_COL32(0, 0, 0, 175), 4.0f);
        dl->AddText(ImVec2(badgePos.x + 8.0f, badgePos.y + 4.0f), accent, badge.c_str());

        Scene* playScene = m_playMode.get_active_scene();
        if (playScene) {
            const bool hasMotion = !playScene->rigidbodyComponents.empty() ||
                                   !playScene->particleEmitterComponents.empty() ||
                                   !playScene->weaponComponents.empty() || m_playScriptLoaded;
            if (!hasMotion) {
                const char* hint = tr("Nada muda aqui — adicione um Corpo Rígido, Partícula ou Script e veja o Play agir",
                                      "Nothing moves here — add a Rigid Body, Particle or Script to see Play in action");
                const ImVec2 hintSize = ImGui::CalcTextSize(hint);
                const ImVec2 hintPos(dispPos.x + dispW * 0.5f - hintSize.x * 0.5f - 8.0f,
                                     dispPos.y + dispH - hintSize.y - 14.0f);
                dl->AddRectFilled(hintPos,
                                  ImVec2(hintPos.x + hintSize.x + 16.0f, hintPos.y + hintSize.y + 8.0f),
                                  IM_COL32(0, 0, 0, 195), 4.0f);
                dl->AddText(ImVec2(hintPos.x + 8.0f, hintPos.y + 4.0f), IM_COL32(235, 235, 240, 255), hint);
            }
        }
    }

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
        // Paint tool (vertex painting): left-drag paints the selected mesh
        // instead of picking. Active while m_paintToolActive is on (the Paint
        // panel toggles it, or the toolbar 'P' button).
        if (m_paintToolActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_paintBrushDown = paint_mesh_stroke(m_editorCamera.position, viewport_mouse_dir(mouse));
            m_activeAxis = GizmoAxis::None;
            m_gizmoDragging = false;
        } else {
            m_paintBrushDown = false;
        }
        // Left click: grab the gizmo axis first, otherwise pick the entity.
        // Select mode has no gizmo — clicks always pick.
        if (!m_paintToolActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_gizmoDragging) {
            if (m_gizmoMode != GizmoMode::Select && gizmo_axis_hit_test(mouse)) {
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
            // Gizmo drag released: the transform change is committed.
            mark_scene_dirty();
        }
    } else if (m_viewportImageHovered && !ImGui::IsAnyMouseDown() && m_gizmoMode != GizmoMode::Select) {
        gizmo_axis_hit_test(mouse); // hover highlight
    }

    // Entity hover tooltip: update the hover-pick pixel every few frames
    // (throttled to avoid a GPU pick pass every frame).
    if (m_viewportImageHovered && !m_gizmoDragging && !ImGui::IsAnyMouseDown()) {
        static int hoverFrame = 0;
        if (++hoverFrame % 3 == 0) {
            m_hoverPickPixel = (mouse - glm::vec2(dispPos.x, dispPos.y)) *
                glm::vec2(static_cast<float>(m_offscreen.width) / dispW,
                          static_cast<float>(m_offscreen.height) / dispH);
            m_hoverPickPending = true;
        }
    } else {
        m_hoverEntityName.clear();
    }
    // Tooltip: show entity name at cursor position when hovering over an entity
    if (!m_hoverEntityName.empty() && m_viewportImageHovered) {
        ImGui::BeginTooltip();
        ImGui::TextColored(ImVec4(0.55f, 0.60f, 1.0f, 1.0f), ICON_FA_CUBE " %s", m_hoverEntityName.c_str());
        ImGui::EndTooltip();
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
    } else if (asset.type == AssetType::Block) {
        // Block model: spawn as a textured cube in front of the camera (the
        // renderer builds the cube mesh + texture pipeline on demand).
        spawn_block_entity(asset.id, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
        std::cout << "[Viewport] Dropped block '" << asset.sourcePath.filename().string()
                  << "' -> spawned block entity\n";
    } else if (asset.type == AssetType::Texture && is_character_texture(asset)) {
        // Minecraft character/mob skin: drop it and the humanoid character
        // spawns in the scene with the skin as its texture — no sidecar, the
        // PNG itself is the character (just like the block flow).
        spawn_character_entity(asset.id, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
        std::cout << "[Viewport] Dropped skin '" << asset.sourcePath.filename().string()
                  << "' -> spawned character entity\n";
    } else if (asset.type == AssetType::Texture && is_block_texture(asset)) {
        // The PNG itself IS a Minecraft-style block (square POT 8-256, not a
        // character/mob skin): drop it straight in the viewport and it becomes
        // the textured cube — no manual "create block model" step needed. The
        // .vblock sidecar is auto-created on first use and reused afterwards.
        const UUID blockId = create_block_asset(asset);
        if (blockId.is_valid()) {
            spawn_block_entity(blockId, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
            std::cout << "[Viewport] Dropped block texture '" << asset.sourcePath.filename().string()
                      << "' -> spawned block entity (vblock " << blockId.to_string() << ")\n";
        }
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

void EditorApplication::draw_gameplay_ui_panel() {
    ImGui::Begin(tr("UI de Gameplay", "Gameplay UI"), &m_showGameplayUi);
    if (!m_showGameplayUi) { ImGui::End(); return; }
    if (m_uiHighContrast) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.02f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    }
    ImGui::TextColored(UI::Colors::Accent, "%s", tr("Pré-visualização UI data-driven", "Data-driven UI preview"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", tr("Esta superfície usa o documento UI público e respeita DPI, safe area e alto contraste.", "This surface consumes the public UI document and respects DPI, safe area and high contrast."));
    ImGui::Text("DPI: %.2fx", m_uiDpiScale);
    ImGui::Text("Safe area: active");
    ImGui::Checkbox(tr("Alto contraste", "High contrast"), &m_uiHighContrast);
    ImGui::SeparatorText(tr("Controles", "Controls"));
    static float health = 0.72f;
    static bool modal = false;
    ImGui::ProgressBar(std::clamp(health, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), tr("Vida", "Health"));
    if (ImGui::Button(tr("Abrir confirmação", "Open confirmation"))) modal = true;
    if (modal) {
        ImGui::OpenPopup(tr("Confirmar ação", "Confirm action"));
        modal = false;
    }
    if (ImGui::BeginPopupModal(tr("Confirmar ação", "Confirm action"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", tr("Deseja confirmar esta ação?", "Do you want to confirm this action?"));
        if (ImGui::Button(tr("Confirmar", "Confirm"))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SeparatorText(tr("Navegação", "Navigation"));
    static int focus = 0;
    const char* items[] = { "Inventário", "Mapa", "Missões", "Configurações" };
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(items[i], focus == i, 0, ImVec2(-1.0f, 28.0f))) focus = i;
        ImGui::PopID();
    }
    if (m_uiHighContrast) ImGui::PopStyleColor(2);
    ImGui::End();
}

void EditorApplication::draw_content_browser_panel() {
    // Local minimum only (see draw_app_bar note about the global style).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(260.0f, 180.0f));
    ImGui::Begin(tr("Assets", "Assets"));
    ImGui::PopStyleVar();

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
        // Auxiliary maps (_n/_s/…) are never blocks: heal sidecars created by
        // older builds right after indexing (idempotent, once per texture),
        // then record sibling material maps on base blocks created before
        // grouping existed.
        for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
            if (candidate.type == AssetType::Texture && is_aux_map_texture(candidate) &&
                !m_auxBlockHealed.contains(candidate.id)) {
                m_auxBlockHealed.insert(candidate.id);
                heal_aux_block_sidecars(candidate);
            }
        }
        enrich_block_material_maps();
        // Sweep orphan .vblock sidecars: files created by older builds but not
        // present in the registry (dead plumbing). They caused the duplicate
        // pile-up (gold_ore_2/3/4/5.vblock) on every drop of the same texture.
        std::error_code sweepEc;
        for (std::filesystem::recursive_directory_iterator it(sourceRoot, sweepEc), end; it != end && !sweepEc; it.increment(sweepEc)) {
            if (!it->is_regular_file()) continue;
            if (it->path().extension().string() != ".vblock") continue;
            if (!m_assetRegistry.find_id(it->path())) {
                std::filesystem::remove(it->path(), sweepEc);
                std::cout << "[ContentBrowser] Removed orphan sidecar '"
                          << it->path().filename().string() << "'" << std::endl;
            }
        }
        const std::filesystem::path registryPath =
            std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
        if (!m_assetRegistry.save(registryPath))
            std::cerr << "[AssetRegistry] Could not persist database: " << registryPath << std::endl;
        indexed = true;
    }

    ImGui::TextDisabled("%s: %s", tr("Pasta do Jogo", "Game Directory"), sourceRoot.string().c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 96.0f);
    ImGui::InputTextWithHint("##AssetSearch", tr("Pesquisar assets...", "Search assets..."), search, sizeof(search));
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FILE_IMPORT "  Importar", ICON_FA_FILE_IMPORT "  Import"), ImVec2(88, 0))) {
        std::string importPath;
        if (pick_file_dialog(importPath, L"Assets (*.*)\0*.*\0", L"Importar Asset", nullptr)) {
            if (m_assetPipeline) {
                const std::filesystem::path cookedRoot =
                    std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
                const ImportResult result = m_assetPipeline->import({ importPath, cookedRoot, 1 });
                if (!result) std::cerr << "[ContentBrowser] " << result.error << std::endl;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_ROTATE "  Atualizar", ICON_FA_ROTATE "  Refresh"), ImVec2(0, 0))) {
        indexed = false;
        m_contentBrowserDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_UPLOAD "  Pack", ICON_FA_UPLOAD "  Pack"), ImVec2(0, 0))) {
        std::string packFolder;
        if (pick_folder_dialog(packFolder, L"Selecionar pasta do pack de texturas")) {
            const size_t count = import_texture_pack(std::filesystem::path(packFolder));
            if (count > 0) {
                indexed = false; // force re-index on next frame
                m_contentBrowserDirty = true;
            }
        }
    }
    // Type filter tabs (the old Combo row could not fit when the dock shrank).
    // "Modelos" groups every 3D/model asset: industry meshes (glTF), block
    // models assembled from Minecraft-style PNGs, and voxel structures.
    const char* filtersPt[] = { "Tudo", "Texturas", "Malhas", "Modelos", "Materiais", "Áudio", "Cenas", "Animações", "Não Usados" };
    const char* filtersEn[] = { "All", "Textures", "Meshes", "Models", "Materials", "Audio", "Scenes", "Animations", "Unused" };
    const float tabAvail = ImGui::GetContentRegionAvail().x;
    const float tabW = ImMax(1.0f, tabAvail / IM_ARRAYSIZE(filtersEn));
    for (int i = 0; i < IM_ARRAYSIZE(filtersEn); ++i) {
        const bool selected = (typeFilter == i);
        if (ImGui::Selectable((m_currentLanguage == EngineLanguage::PT_BR) ? filtersPt[i] : filtersEn[i],
                              selected, 0, ImVec2(tabW, 0))) {
            typeFilter = i;
        }
        if (i + 1 < IM_ARRAYSIZE(filtersEn)) ImGui::SameLine();
    }
    ImGui::Separator();

    std::optional<AssetType> selectedType;
    switch (typeFilter) {
        case 1: selectedType = AssetType::Texture; break;
        case 2: selectedType = AssetType::Mesh; break;
        case 4: selectedType = AssetType::Material; break;
        case 5: selectedType = AssetType::Audio; break;
        case 6: selectedType = AssetType::Scene; break;
        case 7: selectedType = AssetType::Animation; break;
        default: break;
    }

    AssetBrowserModel browser(m_assetRegistry);
    std::vector<AssetMetadata> assets = browser.query(search, selectedType);
    // .vblock sidecars are hidden plumbing: the PNG IS the block, so Block
    // assets never appear as cards (that would duplicate the texture).
    assets.erase(std::remove_if(assets.begin(), assets.end(), [](const AssetMetadata& candidate) {
        return candidate.type == AssetType::Block;
    }), assets.end());
    if (typeFilter == 3) {
        // Modelos: industry meshes + voxel structures + block-capable textures
        // (the PNG itself is the Minecraft-style block) + character/mob skins.
        assets.erase(std::remove_if(assets.begin(), assets.end(), [&](const AssetMetadata& candidate) {
            return candidate.type != AssetType::Mesh && candidate.type != AssetType::VoxelStructure &&
                   !(candidate.type == AssetType::Texture &&
                     (is_block_texture(candidate) || is_character_texture(candidate)));
        }), assets.end());
    }
    if (typeFilter == 8) {
        std::vector<UUID> roots;
        for (const AssetMetadata& candidate : m_assetRegistry.snapshot())
            if (candidate.type == AssetType::Scene) roots.push_back(candidate.id);
        const std::vector<UUID> unused = m_assetRegistry.unused_assets(roots);
        const std::unordered_set<UUID> unusedSet(unused.begin(), unused.end());
        assets.erase(std::remove_if(assets.begin(), assets.end(), [&](const AssetMetadata& candidate) {
            return !unusedSet.contains(candidate.id);
        }), assets.end());
    }
    const auto assetIcon = [](AssetType t) -> const char* {
        switch (t) {
            case AssetType::Texture: return ICON_FA_IMAGE;
            case AssetType::Mesh: return ICON_FA_CUBE;
            case AssetType::Material: return ICON_FA_PAINTBRUSH;
            case AssetType::Audio: return ICON_FA_MUSIC;
            case AssetType::Skeleton: return ICON_FA_SITEMAP;
            case AssetType::Animation: return ICON_FA_FILM;
            case AssetType::Scene: return ICON_FA_CLAPPERBOARD;
            case AssetType::VoxelStructure: return ICON_FA_CUBES;
            case AssetType::Block: return ICON_FA_CUBES;
            default: return ICON_FA_FILE;
        }
    };
    const auto assetTypeName = [this](AssetType t) -> const char* {
        switch (t) {
            case AssetType::Texture: return tr("Textura", "Texture");
            case AssetType::Mesh: return tr("Malha", "Mesh");
            case AssetType::Material: return tr("Material", "Material");
            case AssetType::Audio: return tr("Áudio", "Audio");
            case AssetType::Skeleton: return tr("Esqueleto", "Skeleton");
            case AssetType::Animation: return tr("Animação", "Animation");
            case AssetType::Scene: return tr("Cena", "Scene");
            case AssetType::VoxelStructure: return tr("Voxel", "Voxel");
            case AssetType::Block: return tr("Bloco", "Block");
            default: return "?";
        }
    };

    const float cellSize = 150.0f;
    int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellSize));
    // Lazy loading (like IntersectionObserver in JS): only assets whose cards
    // are inside the browser's visible area ever trigger thumbnail work.
    // Off-screen rows never decode/upload/render, so scrolling a huge folder
    // loads just the screenful instead of the whole list. Row height is an
    // estimate (48px button + type line + wrapped name + spacing) with a
    // one-row safety margin; being slightly generous only costs a few extra
    // small async requests, never a stall.
    const float cellHeight = 96.0f;
    const float scrollY = ImGui::GetScrollY();
    const float windowH = ImGui::GetWindowHeight();
    const int firstVisibleRow = std::max(0, static_cast<int>(scrollY / cellHeight) - 1);
    const int lastVisibleRow = static_cast<int>((scrollY + windowH) / cellHeight) + 1;
    ImGui::Columns(columns, "AssetGrid", false);
    size_t gridIndex = 0;
    for (const AssetMetadata& asset : assets) {
        const int row = static_cast<int>(gridIndex / static_cast<size_t>(std::max(columns, 1)));
        ++gridIndex;
        const bool isVisible = row >= firstVisibleRow && row <= lastVisibleRow;
        const std::string filename = asset.sourcePath.filename().string();
        ImGui::PushID(asset.id.to_string().c_str());

        // Real previews: cooked textures show the actual image (async decode on
        // a worker, cached forever); meshes and blocks render a true 3D
        // thumbnail (offscreen, budgeted a few per frame); audio tiles carry a
        // single active ▶/⏸ preview voice. Only visible cards request work.
        const bool isTexture = (asset.type == AssetType::Texture);
        const bool isAudio = (asset.type == AssetType::Audio);
        const bool isBlockTexture = isTexture && is_block_texture(asset);
        const bool isSkinTexture = isTexture && !isBlockTexture && is_character_texture(asset);
        const bool isModel = (asset.type == AssetType::Mesh);
        const bool isBlockAsset = (asset.type == AssetType::Block);
        VkDescriptorSet thumb = VK_NULL_HANDLE;
        if (isTexture && !isBlockTexture && !isSkinTexture) {
            const auto found = m_assetThumbnails.find(asset.id);
            if (found != m_assetThumbnails.end()) {
                thumb = found->second.imguiId;
            } else if (isVisible) {
                request_asset_thumbnail_decode(asset); // async, 1 upload/frame
            }
        } else if ((isModel || isBlockAsset || isBlockTexture || isSkinTexture) && isVisible) {
            const auto thumbIt = m_asset3dThumbnails.find(asset.id);
            if (thumbIt != m_asset3dThumbnails.end()) {
                thumb = thumbIt->second;
            } else {
                request_3d_thumbnail(asset.id); // rendered by pump_asset_thumbnails
            }
        }
        if (thumb != VK_NULL_HANDLE) {
            if (ImGui::ImageButton("##thumb", thumb, ImVec2(135, 48))) {
                selectedAssetId = asset.id;
                editedImportSettings = asset.importSettings;
            }
        } else {
            if (ImGui::Button(assetIcon(asset.type), ImVec2(135, 48))) {
                selectedAssetId = asset.id;
                editedImportSettings = asset.importSettings;
            }
        }
        if (ImGui::IsItemHovered()) {
            const char* label = isBlockTexture ? tr("Bloco", "Block")
                               : isSkinTexture ? tr("Skin (Personagem/Mob)", "Skin (Character/Mob)")
                               : assetTypeName(asset.type);
            ImGui::SetTooltip("%s\n%s", filename.c_str(), label);
        }
        // Double-click opens the matching editor (industry convention).
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            switch (asset.type) {
                case AssetType::Material: m_specializedEditors.open_editor("Material"); break;
                case AssetType::Animation: m_specializedEditors.open_editor("Animation"); break;
                case AssetType::Audio: m_specializedEditors.open_editor("Audio"); break;
                case AssetType::Mesh: m_wickedTools.showMeshWindow = true; break;
                default: break;
            }
        }
        if (isAudio) {
            const bool playing = m_audioPreviewAsset == asset.id && m_audioPreviewVoice != 0 &&
                                 m_playAudio.is_active(m_audioPreviewVoice);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (135.0f - 42.0f) * 0.5f);
            if (ImGui::Button(playing ? ICON_FA_PAUSE : ICON_FA_PLAY, ImVec2(42, 24))) {
                toggle_audio_preview(asset);
            }
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
            // The PNG is the block: spawn it as a textured cube in the scene
            // (same result as dragging it into the viewport).
            if (isBlockTexture) {
                if (ImGui::MenuItem(tr("Criar Entidade de Bloco na Cena", "Spawn Block in Scene"))) {
                    const UUID blockId = create_block_asset(asset);
                    if (blockId.is_valid()) spawn_block_entity(blockId, m_editorCamera.orbitTarget);
                }
                if (ImGui::MenuItem(tr("Criar Modelo de Bloco (Minecraft)", "Create Block Model (Minecraft)"))) {
                    create_block_asset(asset);
                }
            }
            // The PNG is the character/mob: spawn the humanoid in the scene
            // (same result as dragging it into the viewport).
            if (isSkinTexture) {
                if (ImGui::MenuItem(tr("Criar Personagem na Cena", "Spawn Character in Scene"))) {
                    spawn_character_entity(asset.id, m_editorCamera.orbitTarget);
                }
            }
            // Classification override: the heuristic can misfire on skins vs
            // blocks — the .vblock sidecar is the explicit user mark.
            if (isTexture) {
                if (ImGui::MenuItem(isBlockTexture
                                        ? tr("Desmarcar como Bloco (é personagem/mob?)", "Unmark as Block (character/mob?)")
                                        : tr("Marcar como Bloco de Minecraft", "Mark as Minecraft Block"))) {
                    if (isBlockTexture) unmark_block_texture(asset);
                    else create_block_asset(asset);
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
        if (isBlockTexture) {
            ImGui::TextColored(ImVec4(0.30f, 0.75f, 0.95f, 1.0f), "%s", tr("Bloco", "Block"));
        } else if (isSkinTexture) {
            ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.95f, 1.0f), "%s", tr("Skin", "Skin"));
        } else {
            ImGui::TextColored(UI::Colors::TextSecondary, "%s", assetTypeName(asset.type));
        }
        ImGui::SameLine();
        if (asset.isCooked) ImGui::TextColored(UI::Colors::Success, "%s", ICON_FA_CIRCLE_CHECK);
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
    clamp_floating_window_on_screen();

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
    clamp_floating_window_on_screen();
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
    // Local minimum only: this panel never shrinks below a readable width
    // (the global WindowMinSize stays small so the app bar/shell stay exact).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(290.0f, 160.0f));
    ImGui::Begin(tr("Escultura de Blocos", "Voxel Sculpting Tools"));
    ImGui::PopStyleVar();

    // Responsive rows: label | control when wide, stacked when narrow. The
    // brush settings are written straight into m_activeVoxelBrush (the real
    // operation consumed by paint_voxel_ray) on every change.
    static int shapeIdx = 0;
    const char* shapesPt[] = { "Esfera", "Cubo" };
    const char* shapesEn[] = { "Sphere", "Cube" };
    {
        const bool table = UI::beginPropertyRow(tr("Formato do Pincel", "Brush Shape"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##BrushShape", &shapeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? shapesPt : shapesEn, 2)) {
            m_activeVoxelBrush.shape = static_cast<VoxelBrushShape>(shapeIdx);
        }
        UI::endPropertyRow(table);
    }

    static int modeIdx = 0;
    const char* modesPt[] = { "Colocar Blocos", "Destruir Blocos", "Substituir Blocos" };
    const char* modesEn[] = { "Add Voxels", "Remove Voxels", "Replace Voxels" };
    {
        const bool table = UI::beginPropertyRow(tr("Modo de Ação", "Brush Mode"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##BrushMode", &modeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? modesPt : modesEn, 3)) {
            m_activeVoxelBrush.mode = static_cast<VoxelBrushMode>(modeIdx);
        }
        UI::endPropertyRow(table);
    }

    {
        const bool table = UI::beginPropertyRow(tr("Tamanho do Pincel", "Brush Radius"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##BrushRadius", &m_activeVoxelBrush.radius, 0.5f, 25.0f);
        UI::endPropertyRow(table);
    }

    static int voxelTypeIdx = 1;
    const char* materialsPt[] = { "Grama", "Terra", "Pedra", "Areia", "Madeira", "Vidro", "Pedregulho", "Obsidiana", "Basalto", "Neve" };
    const char* materialsEn[] = { "Grass", "Dirt", "Stone", "Sand", "Wood", "Glass", "Cobblestone", "Obsidian", "Basalt", "Snow" };
    {
        const bool table = UI::beginPropertyRow(tr("Tipo de Bloco", "Voxel Material"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##VoxelType", &voxelTypeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? materialsPt : materialsEn, 10)) {
            m_activeVoxelBrush.voxelType = static_cast<uint16_t>(voxelTypeIdx);
        }
        UI::endPropertyRow(table);
    }

    ImGui::Separator();
    // Paint mode: click/drag in the viewport paints on the selected voxel
    // volume (right button removes). The old "Aplicar Pincel" button used to
    // write settings nobody consumed — painting is now live.
    ImGui::Checkbox(tr("Pintar no viewport (arraste; botão direito remove)", "Paint in viewport (drag; right button removes)"), &m_voxelPaintMode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Com um Mundo de Blocos selecionado, arraste no viewport para esculpir. A ferramenta Select (Q) é usada enquanto o modo de pintura está desligado.", "With a Voxel World selected, drag in the viewport to sculpt. The Select tool (Q) is used while paint mode is off."));
    }

    // Volume actions: generate the terrain from the seed, or clear everything.
    const UUID selectedVolume = (m_selectedEntity.is_valid() && m_editorScene &&
                                 m_editorScene->voxelVolumeComponents.contains(m_selectedEntity.get_id()))
                                    ? m_selectedEntity.get_id()
                                    : UUID{ 0, 0 };
    if (selectedVolume.is_valid()) {
        auto& vol = m_editorScene->voxelVolumeComponents[selectedVolume];
        if (ImGui::Button(tr("Gerar Terreno (semente)", "Generate Terrain (seed)"), ImVec2(-FLT_MIN, 28))) {
            m_voxelStructures.erase(selectedVolume);
            ensure_voxel_volume(selectedVolume, vol.seed, vol.seaLevel);
            m_voxelMeshesDirty.insert(selectedVolume);
        }
        if (ImGui::Button(tr("Limpar Volume", "Clear Volume"), ImVec2(-FLT_MIN, 28))) {
            const auto gridIt = m_voxelStructures.find(selectedVolume);
            if (gridIt != m_voxelStructures.end()) {
                const auto& size = gridIt->second->size();
                for (int x = 0; x < size.x; ++x)
                    for (int y = 0; y < size.y; ++y)
                        for (int z = 0; z < size.z; ++z)
                            gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue::air());
                m_voxelMeshesDirty.insert(selectedVolume);
            }
        }
        ImGui::TextDisabled("%s", tr("Selecione o Mundo de Blocos para esculpir", "Select the Voxel World entity to sculpt"));
    } else {
        ImGui::TextDisabled("%s", tr("Selecione uma entidade 'Mundo de Blocos' (ou crie pelo +Add) para esculpir", "Select a Voxel World entity (or create one via +Add) to sculpt"));
    }

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
        if (m_publishPipeline) m_publishPipeline->fail("no scene open");
        return;
    }
    log("Build started for project '" + m_currentProjectName + "'");
    if (m_publishPipeline) m_publishPipeline->begin(m_currentProjectName);

    // 1. Cook every uncooked asset (same path as the Content Browser).
    size_t imported = 0, failed = 0;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) continue;
        const ImportResult result = m_assetPipeline->import({ asset.sourcePath, cookedRoot, 1 });
        if (result) ++imported;
        else { ++failed; log("  cook failed: " + result.error); }
    }
    log(std::to_string(imported) + " asset(s) cooked, " + std::to_string(failed) + " failed");
    if (m_publishPipeline) m_publishPipeline->cooking_done(imported, failed);

    // 2. Package all cooked assets (Content/<uuid>/<file> + AssetManifest.txt).
    std::vector<UUID> roots;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) roots.push_back(asset.id);
    }
    if (roots.empty()) {
        log("Build failed: no cooked assets to package");
        if (m_publishPipeline) m_publishPipeline->fail("no cooked assets to package");
        return;
    }
    const AssetPackageResult packaged = AssetPackager::package(m_assetRegistry, roots, buildRoot);
    if (!packaged) {
        log("Build failed: " + packaged.error);
        if (m_publishPipeline) m_publishPipeline->fail(packaged.error);
        return;
    }
    log(std::to_string(packaged.assets.size()) + " asset(s) packaged");
    if (m_publishPipeline) m_publishPipeline->packaging_done(packaged.assets.size());

    // 3. Save the authored scene as the game's initial scene.
    std::filesystem::create_directories(buildRoot / "Content" / "Scenes", ec);
    if (!m_editorScene->save_to_file((buildRoot / "Content" / "Scenes" / "Initial.scene").string())) {
        log("Build failed: could not save scene");
        if (m_publishPipeline) m_publishPipeline->fail("could not save scene");
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
    if (m_publishPipeline) m_publishPipeline->publishing_done();
    log("Build complete: " + buildRoot.string());
}

void EditorApplication::draw_console_panel() {
    ImGui::Begin(tr("Console", "Console"));

    ImGui::TextColored(ImVec4(0.39f, 0.40f, 0.95f, 1.00f), "%s", tr("VulkanCraft Engine 1.5.0 - Português (Brasil)", "VulkanCraft Engine 1.5.0 - English (US)"));
    ImGui::Text(tr("Velocidade: %.1f FPS  |  Tempo por Quadro: %.2f ms  |  Memória RAM: %zu MB", "Speed: %.1f FPS  |  Frame Time: %.2f ms  |  RAM: %zu MB"), m_fps, m_frameTimeMs, m_ramUsageMb);
    ImGui::Separator();

    if (!m_buildLog.empty()) {
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("Registro do Build:", "Build Log:"));
        for (const std::string& line : m_buildLog) {
            ImGui::TextWrapped("%s", line.c_str());
        }
        ImGui::Separator();
    }

    // Real device status (queried from the physical device at init).
    // NOTE: the translated string carries its own format specifier, so it must
    // be formatted with snprintf BEFORE ImGui::TextColored (passing it as the
    // "%s" arg would leave the inner %s/%zu literals unexpanded).
    {
        char gpuMsg[512];
        snprintf(gpuMsg, sizeof(gpuMsg), tr("[INFO] Placa de Vídeo Vulkan 1.3 Inicializada: %s", "[INFO] Vulkan 1.3 Device Initialized: %s"), m_gpuName.c_str());
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", gpuMsg);
    }

    // Real scene status: the edited scene and the play world (if playing).
    if (m_editorScene) {
        const size_t entityCount = m_editorScene->get_entities().size();
        char sceneMsg[256];
        snprintf(sceneMsg, sizeof(sceneMsg), tr("[INFO] Cena carregada: %zu entidades", "[INFO] Scene loaded: %zu entities"), entityCount);
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", sceneMsg);
    }
    const PlayState state = m_playMode.get_state();
    if (state == PlayState::Play || state == PlayState::Simulate) {
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("[INFO] Jogo interno em execução (Play In Editor)", "[INFO] In-engine game running (Play In Editor)"));
    }

    // Real asset status from the registry.
    {
        size_t cooked = 0;
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.isCooked) ++cooked;
        }
        char assetMsg[256];
        snprintf(assetMsg, sizeof(assetMsg), tr("[INFO] Registro de assets: %zu total, %zu cozidos", "[INFO] Asset registry: %zu total, %zu cooked"), m_assetRegistry.size(), cooked);
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", assetMsg);
    }

    ImGui::End();
}

// ===========================================================================
// Vulkan helpers for the viewport
// ===========================================================================

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
                                 VkSampleCountFlagBits samples,
                                 bool wireframe, bool depthTest, bool cull, bool withUv,
                                 bool noVertexInput, bool blend,
                                 bool lessOrEqualDepth, bool depthBias,
                                 bool depthWrite,
                                 VkPrimitiveTopology topology) {
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
    vertexInput.vertexBindingDescriptionCount = noVertexInput ? 0u : 1u;
    vertexInput.pVertexBindingDescriptions = noVertexInput ? nullptr : &binding;
    vertexInput.vertexAttributeDescriptionCount = noVertexInput ? 0u : (withUv ? 4u : 3u);
    vertexInput.pVertexAttributeDescriptions = noVertexInput ? nullptr : attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = wireframe ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : topology;
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
    rasterizer.depthBiasEnable = depthBias ? VK_TRUE : VK_FALSE;
    if (depthBias) {
        // Push the grid slightly away so geometry sitting on the plane
        // (y = 0) wins the depth test instead of z-fighting.
        rasterizer.depthBiasConstantFactor = 4.0f;
        rasterizer.depthBiasSlopeFactor = 1.0f;
        rasterizer.depthBiasClamp = 0.0f;
    }

    VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = samples;
    multisampling.alphaToCoverageEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = (depthTest && depthWrite) ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = lessOrEqualDepth ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend) {
        // Premultiplied alpha (the grid is the only blended pipeline; its
        // shader writes vec4(col * alpha, alpha)): ONE / 1-SRC_ALPHA keeps the
        // AA edges clean with no dark fringes and an exact alpha channel.
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
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
// Cube with per-face UVs: 24 vertices / 36 indices. The UVs matter — the
// material-graph pipeline samples textures with the vertex UVs (location 3),
// so block cubes in the scene would otherwise sample the single (0,0) texel
// and render as a solid color ("sem textura").
void build_cube(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices) {
    // BUG-EDITOR-CUBE-WINDING: all six faces MUST share one winding. The scene
    // pipeline (frontFace=CCW + GL-convention projection + positive-height
    // viewport) draws window-CCW triangles, which for this matrix chain means
    // world-CW geometry (normals pointing INWARD). The side faces were already
    // wound that way, but top/bottom were wound outward — so the default cube
    // rendered with an open top and a see-through far side. Top/bottom are now
    // wound exactly like the sides (verified from above/below/side cameras).
    const glm::vec3 n[6] = {
        { 0,  0, -1}, { 0,  0,  1}, {-1,  0,  0},
        { 1,  0,  0}, { 0, -1,  0}, { 0,  1,  0}
    };
    struct FaceVert { glm::vec3 pos; glm::vec2 uv; };
    const FaceVert face[6][4] = {
        { FaceVert{{-0.5f, -0.5f, -0.5f}, {0, 0}}, FaceVert{{ 0.5f, -0.5f, -0.5f}, {1, 0}},
          FaceVert{{ 0.5f,  0.5f, -0.5f}, {1, 1}}, FaceVert{{-0.5f,  0.5f, -0.5f}, {0, 1}} }, // -Z
        { FaceVert{{ 0.5f, -0.5f,  0.5f}, {0, 0}}, FaceVert{{-0.5f, -0.5f,  0.5f}, {1, 0}},
          FaceVert{{-0.5f,  0.5f,  0.5f}, {1, 1}}, FaceVert{{ 0.5f,  0.5f,  0.5f}, {0, 1}} }, // +Z
        { FaceVert{{-0.5f, -0.5f,  0.5f}, {0, 0}}, FaceVert{{-0.5f, -0.5f, -0.5f}, {1, 0}},
          FaceVert{{-0.5f,  0.5f, -0.5f}, {1, 1}}, FaceVert{{-0.5f,  0.5f,  0.5f}, {0, 1}} }, // -X
        { FaceVert{{ 0.5f, -0.5f, -0.5f}, {0, 0}}, FaceVert{{ 0.5f, -0.5f,  0.5f}, {1, 0}},
          FaceVert{{ 0.5f,  0.5f,  0.5f}, {1, 1}}, FaceVert{{ 0.5f,  0.5f, -0.5f}, {0, 1}} }, // +X
        { FaceVert{{-0.5f, -0.5f, -0.5f}, {0, 0}}, FaceVert{{-0.5f, -0.5f,  0.5f}, {0, 1}},
          FaceVert{{ 0.5f, -0.5f,  0.5f}, {1, 1}}, FaceVert{{ 0.5f, -0.5f, -0.5f}, {1, 0}} }, // -Y
        { FaceVert{{-0.5f,  0.5f,  0.5f}, {0, 0}}, FaceVert{{-0.5f,  0.5f, -0.5f}, {0, 1}},
          FaceVert{{ 0.5f,  0.5f, -0.5f}, {1, 1}}, FaceVert{{ 0.5f,  0.5f,  0.5f}, {1, 0}} }, // +Y
    };
    verts.clear();
    indices.clear();
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int c = 0; c < 4; ++c) {
            EditorVertex v;
            v.pos = face[f][c].pos;
            v.normal = n[f];
            v.color = glm::vec3(1.0f);
            v.uv = face[f][c].uv;
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

// ---------------------------------------------------------------------------
// Minecraft-style character geometry (player/mob skin → humanoid): head + body
// + arms + legs boxes, each face UV-mapped to the standard 64x64 skin layout
// (legacy 64x32 reuses the right arm/leg regions for the left side; HD skins
// scale the whole layout, so UVs are normalized from the 64-unit grid). One
// unit = 1/16 m → the character is ~2 m tall. Winding matches build_cube
// (CCW + back-cull), so it renders with the material-graph pipeline.
// ---------------------------------------------------------------------------
struct CharacterUVRect { float u0, v0, u1, v1; };

void append_character_face(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                           const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                           const glm::vec3& d, const glm::vec2& ta, const glm::vec2& tb,
                           const glm::vec2& tc, const glm::vec2& td, const glm::vec3& normal) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    const glm::vec3 p[4] = { a, b, c, d };
    const glm::vec2 t[4] = { ta, tb, tc, td };
    for (int i = 0; i < 4; ++i) {
        EditorVertex v;
        v.pos = p[i];
        v.normal = normal;
        v.color = glm::vec3(1.0f);
        v.uv = t[i];
        verts.push_back(v);
    }
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

// Adds the six faces of a box. `right`/`left` map the +X/-X faces,
// `front`/`back` the +Z/-Z (back flipped so the layout reads correctly),
// `top`/`bottom` the +Y/-Y. UVs come from the 64-unit layout grid and are
// normalized by `skinHeight` (64x64 or legacy 64x32).
void append_character_box(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                          float x0, float y0, float z0, float x1, float y1, float z1,
                          float skinHeight, const CharacterUVRect& right,
                          const CharacterUVRect& left, const CharacterUVRect& front,
                          const CharacterUVRect& back, const CharacterUVRect& top,
                          const CharacterUVRect& bottom) {
    const auto uv = [&](const CharacterUVRect& r, float u, float v) {
        return glm::vec2((r.u0 + (r.u1 - r.u0) * u) / 64.0f,
                         (r.v0 + (r.v1 - r.v0) * v) / skinHeight);
    };
    // +Z front
    append_character_face(verts, indices, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 },
                          { x0, y1, z1 }, uv(front, 0, 1), uv(front, 1, 1), uv(front, 1, 0),
                          uv(front, 0, 0), { 0, 0, 1 });
    // -Z back (u flipped)
    append_character_face(verts, indices, { x1, y0, z0 }, { x0, y0, z0 }, { x0, y1, z0 },
                          { x1, y1, z0 }, uv(back, 1, 1), uv(back, 0, 1), uv(back, 0, 0),
                          uv(back, 1, 0), { 0, 0, -1 });
    // +X right
    append_character_face(verts, indices, { x1, y0, z0 }, { x1, y0, z1 }, { x1, y1, z1 },
                          { x1, y1, z0 }, uv(right, 0, 1), uv(right, 1, 1), uv(right, 1, 0),
                          uv(right, 0, 0), { 1, 0, 0 });
    // -X left (u flipped)
    append_character_face(verts, indices, { x0, y0, z1 }, { x0, y0, z0 }, { x0, y1, z0 },
                          { x0, y1, z1 }, uv(left, 1, 1), uv(left, 0, 1), uv(left, 0, 0),
                          uv(left, 1, 0), { -1, 0, 0 });
    // +Y top (z1 -> v0, the front edge of the top rect)
    append_character_face(verts, indices, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 },
                          { x0, y1, z0 }, uv(top, 0, 0), uv(top, 1, 0), uv(top, 1, 1),
                          uv(top, 0, 1), { 0, 1, 0 });
    // -Y bottom
    append_character_face(verts, indices, { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 },
                          { x0, y0, z1 }, uv(bottom, 0, 1), uv(bottom, 1, 1),
                          uv(bottom, 1, 0), uv(bottom, 0, 0), { 0, -1, 0 });
}

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
} // anonymous namespace

} // namespace Engine
