#pragma once

#include <string>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "../../scene/Scene.hpp"
#include "../../scene/Entity.hpp"
#include "../play_mode/PlayMode.hpp"
#include "../undo/UndoSystem.hpp"
#include "../../assets/AssetRegistry.hpp"
#if VC_ENABLE_VOXEL_PLUGIN
#include "../../../plugins/voxel/VoxelPlugin.hpp"
#endif

namespace Engine {

class EditorGUI {
public:
    EditorGUI() = default;
    EditorGUI(const EditorGUI&) = delete;
    EditorGUI& operator=(const EditorGUI&) = delete;

    void init(Scene* scene, UndoSystem* undoSystem);
    void set_asset_registry(AssetRegistry* registry) { m_assetRegistry = registry; }
    void update(float deltaTime);

    // Window Visibility Toggles
    bool showOutliner{ true };
    bool showInspector{ true };
    bool showContentBrowser{ true };
#if VC_ENABLE_VOXEL_PLUGIN
    bool showVoxelTools{ true };
#else
    bool showVoxelTools{ false };
#endif
    bool showConsole{ true };
    bool showProfiler{ true };

    Entity get_selected_entity() const { return m_selectedEntity; }
    void select_entity(Entity entity) { m_selectedEntity = entity; }

private:
    Scene* m_activeScene{ nullptr };
    UndoSystem* m_undoSystem{ nullptr };
    AssetRegistry* m_assetRegistry{ nullptr };
    Entity m_selectedEntity;

#if VC_ENABLE_VOXEL_PLUGIN
    // Voxel Tool Settings
    VoxelBrushOperation m_activeBrush;
#endif

    void draw_menu_bar();
    void draw_toolbar();
    void draw_world_outliner();
    void draw_inspector();
    void draw_content_browser();
    void draw_voxel_editor_panel();
    void draw_console();
};

} // namespace Engine
