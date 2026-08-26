#pragma once
#include <imgui.h>
#include <string>
#include "../../engine/core/uuid/UUID.hpp"
#include "../../engine/scene/Scene.hpp"
#include "AnimationEditorModel.hpp"
#include "TimelineEditorModel.hpp"
#include "RetargetEditorModel.hpp"
#include "IKEditorModel.hpp"
#include "RagdollEditorModel.hpp"
#include "WeaponEditorModel.hpp"
#include "VehicleEditorModel.hpp"
#include "DestructionEditorModel.hpp"
#include "ParticleEditorModel.hpp"
#include "AudioEditorModel.hpp"
#include "NavigationEditorModel.hpp"
#include "MissionEditorModel.hpp"
#include "DialogueEditorModel.hpp"
#include "PhysicsEditorModel.hpp"
#include "RenderingToolModels.hpp"
namespace Engine::Editor {
class SpecializedEditorsPanel final {
public:
 SpecializedEditorsPanel();
 bool open{false};
 bool previewOnSelected{false};
 void draw();
 // Programmatically opens the panel on the given tab (used by asset
 // double-click: e.g. double-clicking a Material asset opens the Material
 // editor).
 void open_editor(const std::string& tab) { open = true; openTab_ = tab; }
 void set_texture_assets(std::vector<std::pair<std::string, UUID>> assets) { textureAssets_ = std::move(assets); }
 // Scene context for panel → scene integration (Weapon tab applies the
 // authored parameters as a real WeaponComponent on the selected entity).
 void set_scene_context(Scene* scene, UUID selected) { scene_ = scene; selected_ = selected; }
 [[nodiscard]] const Rendering::MaterialGraph& live_material_graph() const noexcept { return materialGraph_; }
 [[nodiscard]] Rendering::MaterialGraph& live_material_graph_mutable() noexcept { return materialGraph_; }
 // Timeline editor state (agente 2 §B l.33): the deterministic document model
 // is mirrored from this real state by refresh_timeline_editor().
    [[nodiscard]] const TimelineEditorModel& live_timeline() const noexcept { return timeline_; }
    [[nodiscard]] const RetargetEditorModel& live_retarget() const noexcept { return retarget_; }
private:
 void draw_validation(const EditorDocumentModel& model);
 ImGuiTabItemFlags tab_flags(const char* name) {
  if (openTab_ == name) { openTab_.clear(); return ImGuiTabItemFlags_SetSelected; }
  return ImGuiTabItemFlags_None;
 }
 std::string openTab_;
 AnimationEditorModel animation_; TimelineEditorModel timeline_; RetargetEditorModel retarget_; IKEditorModel ik_; RagdollEditorModel ragdoll_;
 WeaponEditorModel weapon_; VehicleEditorModel vehicle_; DestructionEditorModel destruction_; ParticleEditorModel particle_; AudioEditorModel audio_;
 NavigationEditorModel navigation_; MissionEditorModel mission_; DialogueEditorModel dialogue_; PhysicsEditorModel physics_;
 Rendering::MaterialGraph materialGraph_; MaterialEditorModel materialEditor_{&materialGraph_};
 Rendering::RenderGraph renderGraph_; RenderGraphViewerModel renderGraphViewer_;
 std::vector<std::pair<std::string, UUID>> textureAssets_;
 Scene* scene_{ nullptr };
 UUID selected_{ 0, 0 };
};
} // namespace Engine::Editor
