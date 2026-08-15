#pragma once
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
 void set_texture_assets(std::vector<std::pair<std::string, UUID>> assets) { textureAssets_ = std::move(assets); }
 // Scene context for panel → scene integration (Weapon tab applies the
 // authored parameters as a real WeaponComponent on the selected entity).
 void set_scene_context(Scene* scene, UUID selected) { scene_ = scene; selected_ = selected; }
 [[nodiscard]] const Rendering::MaterialGraph& live_material_graph() const noexcept { return materialGraph_; }
 [[nodiscard]] Rendering::MaterialGraph& live_material_graph_mutable() noexcept { return materialGraph_; }
private:
 void draw_validation(const EditorDocumentModel& model);
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
