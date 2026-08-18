// ===========================================================================
// WickedToolsPanel — frontend port from the Wicked Engine editor (MIT,
// commit 2aa9fdf, (c) Turánszki János; see src/editor/frontend/PORTS.md).
//
// Policy (per the user decision recorded in PORTS.md):
//   - Feature the VulkanCraft editor HAS        → button wired to ours.
//   - Feature we DON'T have yet                 → panel kept with an explicit
//     TODO(frontend-port) comment, wired when implemented.
//   - Feature the donor DOESN'T have (our own)  → panel created and wired.
// ===========================================================================

#include "WickedToolsPanel.hpp"
#include "../WindowClamp.hpp"
#include <imgui.h>
#include "../frontend/IconsFontAwesome6.h"
#include "../engine/scene/Components.hpp"

namespace Engine {

const char* WickedToolsPanel::tr(const char* pt, const char* en) const {
    return is_pt() ? pt : en;
}

// The editor passes a pointer to its EngineLanguage enum; EN_US is the second
// enumerator, so any non-zero value means English.
bool WickedToolsPanel::is_pt() const {
    return m_language == nullptr;
}

bool WickedToolsPanel::entity_exists(UUID id) const {
    if (!m_scene || !id.is_valid()) return false;
    return m_scene->get_entities().find(id) != m_scene->get_entities().end();
}

void WickedToolsPanel::entity_combo(const char* label, UUID& id, bool includeNone) const {
    const char* currentName = "—";
    if (entity_exists(id)) {
        const auto& entities = m_scene->get_entities();
        const auto it = entities.find(id);
        if (it != entities.end()) currentName = it->second.get_name().c_str();
    } else if (includeNone && !id.is_valid()) {
        currentName = tr("(nenhum)", "(none)");
    }
    if (ImGui::BeginCombo(label, currentName)) {
        if (includeNone && ImGui::Selectable(tr("(nenhum)", "(none)"), !id.is_valid())) {
            id = UUID{ 0, 0 };
        }
        for (const auto& [eid, entity] : m_scene->get_entities()) {
            const bool isSel = (eid == id);
            if (ImGui::Selectable(entity.get_name().c_str(), isSel)) id = eid;
        }
        ImGui::EndCombo();
    }
}

bool WickedToolsPanel::component_panel_begin(const char* title, bool present) {
    if (!present) return false;
    ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.90f, 1.0f), "%s", title);
    ImGui::Separator();
    return true;
}

void WickedToolsPanel::component_panel_end() {
    ImGui::Spacing();
    ImGui::Separator();
}

void WickedToolsPanel::texture_path_input(const char* label, std::string& path) const {
    char buf[512];
    strncpy(buf, path.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    ImGui::InputText(label, buf, sizeof(buf));
    if (ImGui::IsItemDeactivatedAfterEdit()) path = buf;
    if (m_assetRegistry && ImGui::BeginCombo((std::string("##tex") + label).c_str(),
                                             tr("Escolher textura...", "Pick texture..."))) {
        for (const AssetMetadata& meta : m_assetRegistry->snapshot()) {
            if (meta.type == AssetType::Texture) {
                const std::string name = meta.sourcePath.filename().string();
                if (ImGui::Selectable(name.c_str(), name == path)) path = name;
            }
        }
        ImGui::EndCombo();
    }
}

// ===========================================================================
// Windows that edit existing VulkanCraft components (fully wired).
// ===========================================================================

void WickedToolsPanel::draw_name_window() {
    if (!showNameWindow || !m_scene) return;
    ImGui::Begin(tr("Nome do Objeto", "Object Name"), &showNameWindow);
    clamp_floating_window_on_screen();
    if (has_selection()) {
        const auto it = m_scene->get_entities().find(m_selectedEntity);
        if (it != m_scene->get_entities().end()) {
            char buf[256];
            strncpy(buf, it->second.get_name().c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(tr("Nome", "Name"), buf, sizeof(buf))) {
                m_scene->rename_entity(m_selectedEntity, buf);
            }
        }
    } else {
        ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected"));
    }
    ImGui::End();
}

void WickedToolsPanel::draw_layer_window() {
    if (!showLayerWindow || !m_scene) return;
    ImGui::Begin(tr("Camadas", "Layers"), &showLayerWindow);
    clamp_floating_window_on_screen();
    todo_badge("Layers: runtime de camadas (filtros de render/física) é TODO(frontend-port).",
               "Layers: the layer runtime (render/physics filters) is TODO(frontend-port).");
    ImGui::TextWrapped("%s",
        tr("O Wicked agrupa objetos em camadas nomeadas. Aqui as camadas são "
           "estáticas por cena; o runtime de camadas (filtros de render/física) "
           "é TODO(frontend-port).",
           "Wicked groups objects into named layers. Here layers are scene-"
           "static; the layer runtime (render/physics filters) is "
           "TODO(frontend-port)."));
    if (has_selection()) {
        const auto it = m_scene->get_entities().find(m_selectedEntity);
        if (it != m_scene->get_entities().end()) {
            static int layer = 0;
            ImGui::SliderInt(tr("Camada do Objeto", "Object Layer"), &layer, 0, 31);
        }
    }
    ImGui::End();
}

void WickedToolsPanel::draw_object_window() {
    if (!showObjectWindow || !m_scene) return;
    ImGui::Begin(tr("Objeto", "Object"), &showObjectWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) {
        ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected"));
        ImGui::End();
        return;
    }
    const auto it = m_scene->get_entities().find(m_selectedEntity);
    if (it == m_scene->get_entities().end()) { ImGui::End(); return; }

    char buf[256];
    strncpy(buf, it->second.get_name().c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (ImGui::InputText(tr("Nome", "Name"), buf, sizeof(buf))) {
        m_scene->rename_entity(m_selectedEntity, buf);
    }
    ImGui::TextDisabled("%s: %s", tr("Código Único", "UUID"), m_selectedEntity.to_string().c_str());
    ImGui::Separator();

    auto& t = m_scene->transformComponents[m_selectedEntity];
    ImGui::DragFloat3(tr("Posição", "Position"), &t.position.x, 0.05f);
    ImGui::DragFloat3(tr("Rotação (graus)", "Rotation (deg)"), &t.rotation.x, 0.5f);
    ImGui::DragFloat3(tr("Escala", "Scale"), &t.scale.x, 0.05f, 0.01f, 1000.0f);

    ImGui::Separator();
    if (ImGui::Button(tr("Duplicar Objeto", "Duplicate Object"))) {
        // The editor scene has no clone API on Entity; the closest is to copy
        // the transform onto a fresh entity. TODO(frontend-port): full entity
        // duplication (components + children) once Scene exposes it.
        Entity copy = m_scene->create_entity(it->second.get_name() + " (cópia)");
        const UUID newId = copy.get_id();
        if (m_scene->transformComponents.contains(m_selectedEntity)) {
            m_scene->transformComponents[newId] = m_scene->transformComponents[m_selectedEntity];
        }
    }
    if (ImGui::Button(tr("Remover Objeto", "Delete Object"), ImVec2(0, 0))) {
        const UUID doomed = m_selectedEntity;
        m_scene->destroy_entity(doomed);
        m_selectedEntity = UUID{ 0, 0 };
        if (m_onEntityDeleted) m_onEntityDeleted(doomed);
    }
    ImGui::End();
}

void WickedToolsPanel::draw_light_window() {
    if (!showLightWindow || !m_scene) return;
    ImGui::Begin(tr("Luz", "Light"), &showLightWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->lightComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Luz ao Objeto", "Add Light to Object"))) {
            m_scene->lightComponents[id] = LightComponent{};
        }
        ImGui::End();
        return;
    }
    if (!component_panel_begin(tr("Iluminação e Luz", "Light Component"), true)) { ImGui::End(); return; }
    auto& l = m_scene->lightComponents[id];
    ImGui::ColorEdit3(tr("Cor da Luz", "Light Color"), &l.color.r);
    ImGui::DragFloat(tr("Brilho (Intensidade)", "Intensity"), &l.intensity, 10.0f, 0.0f, 100000.0f);
    ImGui::DragFloat(tr("Alcance da Luz", "Range"), &l.range, 0.5f, 0.1f, 10000.0f);
    ImGui::Checkbox(tr("Projetar Sombras", "Cast Shadows"), &l.castShadows);
    const char* types[] = { "Directional", "Point", "Spot", "Area" };
    int typeIdx = static_cast<int>(l.type);
    if (ImGui::Combo(tr("Tipo", "Type"), &typeIdx, types, 4)) l.type = static_cast<LightType>(typeIdx);
    if (ImGui::Button(tr("Remover Luz", "Remove Light"))) m_scene->lightComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_camera_window() {
    if (!showCameraWindow || !m_scene) return;
    ImGui::Begin(tr("Câmera", "Camera"), &showCameraWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->cameraComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Câmera ao Objeto", "Add Camera to Object"))) {
            m_scene->cameraComponents[id] = CameraComponent{};
        }
        ImGui::End();
        return;
    }
    if (!component_panel_begin(tr("Câmera de Visão", "Camera Component"), true)) { ImGui::End(); return; }
    auto& c = m_scene->cameraComponents[id];
    ImGui::SliderFloat(tr("Campo de Visão (FOV)", "Field of View (FOV)"), &c.fov, 10.0f, 160.0f);
    ImGui::DragFloat(tr("Visão Próxima", "Near Plane"), &c.nearPlane, 0.01f, 0.001f, 100.0f);
    ImGui::DragFloat(tr("Visão Distante", "Far Plane"), &c.farPlane, 10.0f, 10.0f, 100000.0f);
    ImGui::Checkbox(tr("Câmera Principal do Jogo", "Primary Camera"), &c.isPrimary);
    if (ImGui::Button(tr("Remover Câmera", "Remove Camera"))) m_scene->cameraComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_material_window() {
    if (!showMaterialWindow || !m_scene) return;
    ImGui::Begin(tr("Material", "Material"), &showMaterialWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->materialComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Material ao Objeto", "Add Material to Object"))) {
            m_scene->materialComponents[id] = MaterialComponent{};
        }
        ImGui::End();
        return;
    }
    if (!component_panel_begin(tr("Material", "Material Component"), true)) { ImGui::End(); return; }
    auto& m = m_scene->materialComponents[id];
    ImGui::ColorEdit3(tr("Cor (Albedo)", "Albedo"), &m.albedo.r);
    ImGui::SliderFloat(tr("Rugosidade", "Roughness"), &m.roughness, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Metal", "Metallic"), &m.metallic, 0.0f, 1.0f);
    ImGui::ColorEdit3(tr("Cor Emissiva", "Emissive Color"), &m.emissiveColor.r);
    ImGui::DragFloat(tr("Intensidade Emissiva", "Emissive Intensity"), &m.emissiveIntensity, 0.1f, 0.0f, 10000.0f);
    if (m_openSpecializedEditors && ImGui::Button(tr("Abrir Editor de Materiais...", "Open Material Editor..."))) {
        *m_openSpecializedEditors = true;
    }
    if (ImGui::Button(tr("Remover Material", "Remove Material"))) m_scene->materialComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_sound_window() {
    if (!showSoundWindow || !m_scene) return;
    ImGui::Begin(tr("Som", "Sound"), &showSoundWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->audioComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Som ao Objeto", "Add Sound to Object"))) {
            m_scene->audioComponents[id] = AudioComponent{};
        }
        ImGui::End();
        return;
    }
    if (!component_panel_begin(tr("Fonte de Áudio", "Audio Component"), true)) { ImGui::End(); return; }
    auto& a = m_scene->audioComponents[id];
    texture_path_input(tr("Clip (.ogg)", "Clip (.ogg)"), a.clipPath);
    ImGui::SliderFloat(tr("Volume", "Volume"), &a.volume, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Tom (Pitch)", "Pitch"), &a.pitch, 0.1f, 4.0f);
    ImGui::Checkbox(tr("Espacial (3D)", "Spatial (3D)"), &a.spatial);
    ImGui::Checkbox(tr("Em Loop", "Looping"), &a.looping);
    ImGui::Checkbox(tr("Tocar ao Iniciar", "Play on Start"), &a.playOnStart);
    if (ImGui::Button(tr("Remover Som", "Remove Sound"))) m_scene->audioComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_rigidbody_window() {
    if (!showRigidBodyWindow || !m_scene) return;
    ImGui::Begin(tr("Corpo Rígido", "Rigid Body"), &showRigidBodyWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->rigidbodyComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Corpo Rígido", "Add Rigid Body"))) {
            m_scene->rigidbodyComponents[id] = RigidbodyComponent{};
        }
        ImGui::End();
        return;
    }
    if (!component_panel_begin(tr("Física e Gravidade", "Rigidbody Component"), true)) { ImGui::End(); return; }
    auto& r = m_scene->rigidbodyComponents[id];
    ImGui::DragFloat(tr("Peso (kg)", "Mass (kg)"), &r.mass, 0.1f, 0.01f, 100000.0f);
    ImGui::SliderFloat(tr("Deslize (Fricção)", "Friction"), &r.friction, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Quique (Elasticidade)", "Restitution"), &r.restitution, 0.0f, 1.0f);
    ImGui::Checkbox(tr("Física Fixa (Sem Mover)", "Is Kinematic"), &r.isKinematic);
    ImGui::Checkbox(tr("Ativar Gravidade", "Use Gravity"), &r.useGravity);
    if (ImGui::Button(tr("Remover Corpo Rígido", "Remove Rigid Body"))) m_scene->rigidbodyComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_collider_window() {
    if (!showColliderWindow || !m_scene) return;
    ImGui::Begin(tr("Colisor", "Collider"), &showColliderWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->colliderComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Colisor", "Add Collider"))) {
            m_scene->colliderComponents[id] = ColliderComponent{};
        }
        ImGui::End();
        return;
    }
    if (!component_panel_begin(tr("Colisor", "Collider Component"), true)) { ImGui::End(); return; }
    auto& c = m_scene->colliderComponents[id];
    const char* shapes[] = { "Caixa (Box)", "Esfera (Sphere)", "Cápsula (Capsule)" };
    int shapeIdx = static_cast<int>(c.shape);
    if (ImGui::Combo(tr("Forma", "Shape"), &shapeIdx, shapes, 3)) c.shape = static_cast<ColliderShape>(shapeIdx);
    if (c.shape == ColliderShape::Box) {
        ImGui::DragFloat3(tr("Tamanho", "Size"), &c.size.x, 0.05f, 0.01f, 1000.0f);
    } else if (c.shape == ColliderShape::Sphere) {
        ImGui::DragFloat(tr("Raio", "Radius"), &c.radius, 0.05f, 0.01f, 1000.0f);
    } else {
        ImGui::DragFloat(tr("Raio", "Radius"), &c.radius, 0.05f, 0.01f, 1000.0f);
        ImGui::DragFloat(tr("Altura", "Height"), &c.height, 0.05f, 0.01f, 1000.0f);
    }
    ImGui::DragFloat3(tr("Deslocamento", "Offset"), &c.offset.x, 0.05f);
    ImGui::Checkbox(tr("Apenas Gatilho (Trigger)", "Is Trigger"), &c.isTrigger);
    ImGui::Checkbox(tr("Ativo", "Enabled"), &c.enabled);
    ImGui::TextWrapped("%s", tr("O play world mapeia este colisor na forma de "
        "física do corpo rígido. TODO(frontend-port): shape autorizada no play "
        "world (Jolt/Bullet).",
        "The play world maps this collider onto the rigidbody's physics shape. "
        "TODO(frontend-port): authored shape in the play world (Jolt/Bullet)."));
    if (ImGui::Button(tr("Remover Colisor", "Remove Collider"))) m_scene->colliderComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

// ===========================================================================
// Windows that edit the Wicked-port components (authored data; runtime
// integration marked per struct in Components.hpp / TODO(frontend-port)).
// ===========================================================================

void WickedToolsPanel::draw_constraint_window() {
    if (!showConstraintWindow || !m_scene) return;
    ImGui::Begin(tr("Vínculo (Constraint)", "Constraint"), &showConstraintWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->constraintComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Vínculo", "Add Constraint"))) m_scene->constraintComponents[id] = ConstraintComponent{};
        ImGui::End(); return;
    }
    if (!component_panel_begin(tr("Vínculo de Física", "Constraint Component"), true)) { ImGui::End(); return; }
    auto& c = m_scene->constraintComponents[id];
    const char* types[] = { "Fixo (Fixed)", "Dobradiça (Hinge)", "Mola (Spring)", "Ponto (Point)" };
    int typeIdx = static_cast<int>(c.type);
    if (ImGui::Combo(tr("Tipo", "Type"), &typeIdx, types, 4)) c.type = static_cast<ConstraintType>(typeIdx);
    entity_combo(tr("Outro Objeto", "Other Object"), c.otherEntity);
    ImGui::DragFloat3(tr("Âncora", "Anchor"), &c.anchor.x, 0.05f);
    ImGui::DragFloat3(tr("Eixo", "Axis"), &c.axis.x, 0.05f);
    ImGui::DragFloat(tr("Força de Ruptura (0 = inquebrável)", "Break Force (0 = unbreakable)"), &c.breakForce, 1.0f, 0.0f, 1e9f);
    ImGui::Checkbox(tr("Ativo", "Enabled"), &c.enabled);
    ImGui::TextWrapped("%s", tr("No play world, vira uma joint real (Fixo/Dobradiça/Mola/Ponto) entre os dois corpos.",
        "In play mode this becomes a real joint (Fixed/Hinge/Spring/Point) between the two bodies."));
    if (ImGui::Button(tr("Remover Vínculo", "Remove Constraint"))) m_scene->constraintComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_softbody_window() {
    if (!showSoftBodyWindow || !m_scene) return;
    ImGui::Begin(tr("Corpo Mole (Soft Body)", "Soft Body"), &showSoftBodyWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->softBodyComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Corpo Mole", "Add Soft Body"))) m_scene->softBodyComponents[id] = SoftBodyComponent{};
        ImGui::End(); return;
    }
    todo_badge("Soft body: solver no backend de física (tecidos/trapos) é TODO(frontend-port).",
               "Soft body: physics-backend solver (cloth/rags) is TODO(frontend-port).");
    if (!component_panel_begin(tr("Corpo Mole (Tecido)", "Soft Body Component"), true)) { ImGui::End(); return; }
    auto& s = m_scene->softBodyComponents[id];
    ImGui::DragInt(tr("Detalhe", "Detail"), reinterpret_cast<int*>(&s.detail), 1, 2, 64);
    ImGui::DragFloat(tr("Massa", "Mass"), &s.mass, 0.1f, 0.01f, 1000.0f);
    ImGui::SliderFloat(tr("Fricção", "Friction"), &s.friction, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Elasticidade", "Restitution"), &s.restitution, 0.0f, 1.0f);
    ImGui::DragFloat(tr("Pressão", "Pressure"), &s.pressure, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat(tr("Raio do Vértice", "Vertex Radius"), &s.vertexRadius, 0.005f, 0.001f, 1.0f);
    ImGui::Checkbox(tr("Vento", "Wind"), &s.wind);
    ImGui::TextWrapped("%s", tr("TODO(frontend-port): solver de corpo mole no backend de física (tecidos/trapos).",
        "TODO(frontend-port): soft body solver in the physics backend (cloth/rags)."));
    if (ImGui::Button(tr("Remover Corpo Mole", "Remove Soft Body"))) m_scene->softBodyComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_spring_window() {
    if (!showSpringWindow || !m_scene) return;
    ImGui::Begin(tr("Mola (Spring)", "Spring"), &showSpringWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->springComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Mola", "Add Spring"))) m_scene->springComponents[id] = SpringComponent{};
        ImGui::End(); return;
    }
    if (!component_panel_begin(tr("Dinâmica de Mola", "Spring Component"), true)) { ImGui::End(); return; }
    auto& s = m_scene->springComponents[id];
    ImGui::SliderFloat(tr("Rigidez", "Stiffness"), &s.stiffness, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Arrasto", "Drag"), &s.drag, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Vento", "Wind"), &s.wind, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Gravidade", "Gravity"), &s.gravity, 0.0f, 1.0f);
    ImGui::DragFloat(tr("Raio de Impacto", "Hit Radius"), &s.hitRadius, 0.05f, 0.0f, 10.0f);
    ImGui::Checkbox(tr("Desativada", "Disabled"), &s.disabled);
    ImGui::Checkbox(tr("Usar Gravidade", "Use Gravity"), &s.useGravity);
    ImGui::TextWrapped("%s", tr("No play world, a mola puxa o corpo de volta ao repouso (Hooke + amortecimento).",
        "In play mode the spring pulls the body back to rest (Hooke + damping)."));
    if (ImGui::Button(tr("Remover Mola", "Remove Spring"))) m_scene->springComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_decal_window() {
    if (!showDecalWindow || !m_scene) return;
    ImGui::Begin(tr("Decalque (Decal)", "Decal"), &showDecalWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->decalComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Decalque", "Add Decal"))) m_scene->decalComponents[id] = DecalComponent{};
        ImGui::End(); return;
    }
    todo_badge("Decal: pass de decal no renderer é TODO(frontend-port).",
               "Decal: decal pass in the renderer is TODO(frontend-port).");
    if (!component_panel_begin(tr("Textura Projetada", "Decal Component"), true)) { ImGui::End(); return; }
    auto& d = m_scene->decalComponents[id];
    texture_path_input(tr("Textura", "Texture"), d.texturePath);
    ImGui::ColorEdit3(tr("Cor", "Color"), &d.color.r);
    ImGui::DragFloat(tr("Suavidade da Inclinação", "Slope Blend Power"), &d.slopeBlendPower, 0.1f, 0.0f, 10.0f);
    ImGui::Checkbox(tr("Projetar em Estáticos", "Project on Static"), &d.projectOnStatic);
    ImGui::Checkbox(tr("Apenas Alfa", "Only Alpha"), &d.onlyAlpha);
    ImGui::TextWrapped("%s", tr("TODO(frontend-port): pass de decal no renderer.",
        "TODO(frontend-port): decal pass in the renderer."));
    if (ImGui::Button(tr("Remover Decalque", "Remove Decal"))) m_scene->decalComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_emitter_window() {
    if (!showEmitterWindow || !m_scene) return;
    ImGui::Begin(tr("Emissor de Partículas", "Particle Emitter"), &showEmitterWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->particleEmitterComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Emissor", "Add Emitter"))) m_scene->particleEmitterComponents[id] = ParticleEmitterComponent{};
        ImGui::End(); return;
    }
    if (!component_panel_begin(tr("Emissor de Partículas", "Particle Emitter Component"), true)) { ImGui::End(); return; }
    auto& p = m_scene->particleEmitterComponents[id];
    ImGui::DragFloat3(tr("Posição (local)", "Position (local)"), &p.position.x, 0.05f);
    ImGui::DragFloat3(tr("Direção", "Direction"), &p.direction.x, 0.05f);
    ImGui::SliderFloat(tr("Cone (rad)", "Cone (rad)"), &p.coneAngle, 0.0f, 1.5f);
    ImGui::DragFloat(tr("Taxa (part/s)", "Rate (part/s)"), &p.rate, 1.0f, 0.0f, 100000.0f);
    ImGui::DragFloat(tr("Vel. min", "Speed Min"), &p.speedMin, 0.1f, 0.0f, 1000.0f);
    ImGui::DragFloat(tr("Vel. máx", "Speed Max"), &p.speedMax, 0.1f, 0.0f, 1000.0f);
    ImGui::DragFloat(tr("Vida min (s)", "Lifetime Min"), &p.lifetimeMin, 0.05f, 0.01f, 60.0f);
    ImGui::DragFloat(tr("Vida máx (s)", "Lifetime Max"), &p.lifetimeMax, 0.05f, 0.01f, 60.0f);
    ImGui::DragFloat(tr("Tamanho inicial", "Size Start"), &p.sizeStart, 0.01f, 0.0f, 100.0f);
    ImGui::DragFloat(tr("Tamanho final", "Size End"), &p.sizeEnd, 0.01f, 0.0f, 100.0f);
    ImGui::ColorEdit4(tr("Cor inicial", "Start Color"), &p.colorStart.x);
    ImGui::ColorEdit4(tr("Cor final", "End Color"), &p.colorEnd.x);
    ImGui::DragFloat3(tr("Aceleração", "Acceleration"), &p.acceleration.x, 0.1f);
    ImGui::SliderFloat(tr("Arrasto", "Drag"), &p.drag, 0.0f, 1.0f);
    ImGui::DragFloat(tr("Turbulência", "Turbulence"), &p.turbulence, 0.01f, 0.0f, 10.0f);
    ImGui::DragInt(tr("Rajada no início", "Burst on start"), reinterpret_cast<int*>(&p.burstCount), 1, 0, 1000000);
    ImGui::Checkbox(tr("Colide com física", "Collides with physics"), &p.collide);
    ImGui::Checkbox(tr("Emitindo", "Emitting"), &p.emitting);
    if (ImGui::Button(tr("Remover Emissor", "Remove Emitter"))) m_scene->particleEmitterComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_hair_particle_window() {
    if (!showHairParticleWindow || !m_scene) return;
    ImGui::Begin(tr("Cabelo / Fios (Hair)", "Hair Particles"), &showHairParticleWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->hairParticleComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Cabelo", "Add Hair"))) m_scene->hairParticleComponents[id] = HairParticleComponent{};
        ImGui::End(); return;
    }
    todo_badge("Hair: render de fios (strands) é TODO(frontend-port).",
               "Hair: strand rendering is TODO(frontend-port).");
    if (!component_panel_begin(tr("Sistema de Fios", "Hair Particle Component"), true)) { ImGui::End(); return; }
    auto& h = m_scene->hairParticleComponents[id];
    texture_path_input(tr("Mesh (cabeça)", "Mesh (head)"), h.meshPath);
    ImGui::DragInt(tr("Quantidade", "Count"), reinterpret_cast<int*>(&h.count), 10, 1, 1000000);
    ImGui::DragFloat(tr("Comprimento", "Length"), &h.length, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat(tr("Espessura", "Width"), &h.width, 0.001f, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Rigidez", "Stiffness"), &h.stiffness, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Arrasto", "Drag"), &h.drag, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Gravidade", "Gravity Power"), &h.gravityPower, 0.0f, 5.0f);
    ImGui::SliderFloat(tr("Aleatoriedade", "Randomness"), &h.randomness, 0.0f, 1.0f);
    ImGui::DragInt(tr("Segmentos", "Segments"), reinterpret_cast<int*>(&h.segments), 1, 1, 32);
    ImGui::DragInt(tr("Semente", "Random Seed"), reinterpret_cast<int*>(&h.seed), 1);
    ImGui::TextWrapped("%s", tr("TODO(frontend-port): render de fios (strands) no renderer.",
        "TODO(frontend-port): strand rendering in the renderer."));
    if (ImGui::Button(tr("Remover Cabelo", "Remove Hair"))) m_scene->hairParticleComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_spline_window() {
    if (!showSplineWindow || !m_scene) return;
    ImGui::Begin(tr("Curva (Spline)", "Spline"), &showSplineWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->splineComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Curva", "Add Spline"))) m_scene->splineComponents[id] = SplineComponent{};
        ImGui::End(); return;
    }
    if (!component_panel_begin(tr("Curva Catmull-Rom", "Spline Component"), true)) { ImGui::End(); return; }
    auto& s = m_scene->splineComponents[id];
    ImGui::Checkbox(tr("Fechada (Loop)", "Looped"), &s.looped);
    ImGui::Checkbox(tr("Preenchida", "Filled"), &s.filled);
    ImGui::DragFloat(tr("Largura", "Width"), &s.width, 0.05f, 0.01f, 1000.0f);
    ImGui::DragFloat(tr("Rotação", "Rotation"), &s.rotation, 0.5f);
    ImGui::DragInt(tr("Subdivisões", "Subdivisions"), reinterpret_cast<int*>(&s.subdiv), 1, 2, 256);
    ImGui::Separator();
    ImGui::TextDisabled("%s (%zu)", tr("Pontos de controle", "Control points"), s.points.size());
    for (size_t i = 0; i < s.points.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::DragFloat3(tr("Ponto", "Point"), &s.points[i].x, 0.05f);
        if (ImGui::Button(tr("Remover", "Remove"))) { s.points.erase(s.points.begin() + static_cast<ptrdiff_t>(i)); --i; }
        ImGui::PopID();
    }
    if (ImGui::Button(tr("Adicionar Ponto", "Add Point"))) s.points.push_back(s.points.empty() ? glm::vec3(0.0f) : s.points.back());
    ImGui::TextWrapped("%s", tr("No play world, a entidade segue a curva (follower Catmull-Rom) em loop.",
        "In play mode the entity follows the Catmull-Rom curve (looped)."));
    if (ImGui::Button(tr("Remover Curva", "Remove Spline"))) m_scene->splineComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_force_field_window() {
    if (!showForceFieldWindow || !m_scene) return;
    ImGui::Begin(tr("Campo de Força", "Force Field"), &showForceFieldWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->forceFieldComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Campo de Força", "Add Force Field"))) m_scene->forceFieldComponents[id] = ForceFieldComponent{};
        ImGui::End(); return;
    }
    if (!component_panel_begin(tr("Zona de Força", "Force Field Component"), true)) { ImGui::End(); return; }
    auto& f = m_scene->forceFieldComponents[id];
    const char* types[] = { "Gravidade", "Empurrão (Push)", "Vento (Wind)", "Vórtice (Vortex)" };
    int typeIdx = static_cast<int>(f.type);
    if (ImGui::Combo(tr("Tipo", "Type"), &typeIdx, types, 4)) f.type = static_cast<ForceFieldType>(typeIdx);
    ImGui::DragFloat(tr("Força", "Strength"), &f.strength, 0.1f, -1000.0f, 1000.0f);
    ImGui::DragFloat(tr("Alcance", "Range"), &f.range, 0.5f, 0.1f, 10000.0f);
    ImGui::TextWrapped("%s", tr("No play world, a zona aplica força aos corpos rígidos dentro do alcance (Gravidade/Empurrão/Vento/Vórtice).",
        "In play mode the zone applies force to rigid bodies within range (Gravity/Push/Wind/Vortex)."));
    if (ImGui::Button(tr("Remover Campo de Força", "Remove Force Field"))) m_scene->forceFieldComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_env_probe_window() {
    if (!showEnvProbeWindow || !m_scene) return;
    ImGui::Begin(tr("Sonda Ambiental (Env Probe)", "Environment Probe"), &showEnvProbeWindow);
    clamp_floating_window_on_screen();
    if (!has_selection()) { ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected")); ImGui::End(); return; }
    const UUID id = m_selectedEntity;
    if (!m_scene->envProbeComponents.contains(id)) {
        if (ImGui::Button(tr("Adicionar Sonda", "Add Env Probe"))) m_scene->envProbeComponents[id] = EnvProbeComponent{};
        ImGui::End(); return;
    }
    todo_badge("Env Probe: captura de cubemap é TODO(frontend-port).",
               "Env Probe: cubemap capture is TODO(frontend-port).");
    if (!component_panel_begin(tr("Reflexão Ambiental", "Env Probe Component"), true)) { ImGui::End(); return; }
    auto& e = m_scene->envProbeComponents[id];
    ImGui::Checkbox(tr("Tempo Real", "Real Time"), &e.realTime);
    ImGui::DragFloat(tr("Distância de Visão", "View Distance"), &e.viewDistance, 1.0f, 1.0f, 10000.0f);
    const char* res[] = { "128", "256", "512", "1024" };
    int resIdx = 0;
    for (int i = 0; i < 4; ++i) if (e.resolution == static_cast<uint32_t>(128 << i)) resIdx = i;
    if (ImGui::Combo(tr("Resolução", "Resolution"), &resIdx, res, 4)) e.resolution = static_cast<uint32_t>(128 << resIdx);
    ImGui::TextWrapped("%s", tr("TODO(frontend-port): captura de cubemap no renderer.",
        "TODO(frontend-port): cubemap capture pass in the renderer."));
    if (ImGui::Button(tr("Remover Sonda", "Remove Env Probe"))) m_scene->envProbeComponents.erase(id);
    component_panel_end();
    ImGui::End();
}

void WickedToolsPanel::draw_weather_window() {
    if (!showWeatherWindow || !m_scene) return;
    ImGui::Begin(tr("Clima e Céu", "Weather & Sky"), &showWeatherWindow);
    clamp_floating_window_on_screen();
    // Weather is scene-global: operate on a dedicated "Weather" entity that the
    // panel creates on first use (same pattern as the donor's scene weather).
    UUID weatherId{ 0, 0 };
    for (const auto& [id, entity] : m_scene->get_entities()) {
        if (m_scene->weatherComponents.contains(id)) { weatherId = id; break; }
    }
    if (!weatherId.is_valid()) {
        if (ImGui::Button(tr("Criar Clima da Cena", "Create Scene Weather"))) {
            Entity w = m_scene->create_entity("Weather");
            m_scene->weatherComponents[w.get_id()] = WeatherComponent{};
        }
        ImGui::TextWrapped("%s", tr("O clima é global por cena; crie-o acima e ajuste aqui.",
            "Weather is scene-global; create it above and tune it here."));
        ImGui::End(); return;
    }
    if (!component_panel_begin(tr("Clima Global", "Weather Component"), true)) { ImGui::End(); return; }
    auto& w = m_scene->weatherComponents[weatherId];
    ImGui::ColorEdit3(tr("Cor do Sol", "Sun Color"), &w.sunColor.r);
    ImGui::DragFloat(tr("Densidade da Névoa", "Fog Density"), &w.fogDensity, 0.0001f, 0.0f, 1.0f);
    ImGui::DragFloat(tr("Início da Névoa", "Fog Start"), &w.fogStart, 1.0f, 0.0f, 100000.0f);
    ImGui::SliderFloat(tr("Exposição do Céu", "Sky Exposure"), &w.skyExposure, 0.1f, 10.0f);
    ImGui::SliderAngle(tr("Rotação do Céu", "Sky Rotation"), &w.skyRotation);
    ImGui::DragFloat(tr("Velocidade do Vento", "Wind Speed"), &w.windSpeed, 0.1f, 0.0f, 500.0f);
    ImGui::SliderFloat(tr("Quantidade de Chuva", "Rain Amount"), &w.rainAmount, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Comprimento da Chuva", "Rain Length"), &w.rainLength, 0.1f, 10.0f);
    ImGui::Checkbox(tr("Névoa por Altura", "Height Fog"), &w.heightFog);
    ImGui::TextWrapped("%s", tr("O céu procedural (sol/lua/estrelas/nuvens) é renderizado no viewport a partir destes valores.",
        "The procedural sky (sun/moon/stars/clouds) renders in the viewport from these values."));
    if (ImGui::Button(tr("Remover Clima da Cena", "Remove Scene Weather"))) m_scene->weatherComponents.erase(weatherId);
    component_panel_end();
    ImGui::End();
}

// ===========================================================================
// Animation-related windows.
// ===========================================================================

void WickedToolsPanel::draw_animation_window() {
    if (!showAnimationWindow) return;
    ImGui::Begin(tr("Animação", "Animation"), &showAnimationWindow);
    clamp_floating_window_on_screen();
    todo_badge("Animation: playback de clips no play mode é TODO(frontend-port).",
               "Animation: clip playback in play mode is TODO(frontend-port).");
    ImGui::TextWrapped("%s", tr(
        "Playback de animações: a engine tem AnimationClip/SkeletonAsset com "
        "sampler, blender, state machine e retargeter (src/engine/animation). "
        "TODO(frontend-port): ligar este painel ao runtime de animação do "
        "editor (carregar clips do asset registry e dirigir um AnimationSampler "
        "na cena); os controles abaixo já refletem o modelo de dados.",
        "Animation playback: the engine has AnimationClip/SkeletonAsset with "
        "sampler, blender, state machine and retargeter (src/engine/animation). "
        "TODO(frontend-port): wire this panel to the editor animation runtime "
        "(load clips from the asset registry and drive an AnimationSampler in "
        "the scene); the controls below already mirror the data model."));
    ImGui::TextDisabled("%s", tr("Modo: Clip", "Mode: Clip"));
    ImGui::TextDisabled("%s", tr("Loop: Sim", "Loop: Yes"));
    if (ImGui::Button((std::string(ICON_FA_PLAY) + "  " + tr("Tocar", "Play")).c_str())) { /* TODO(frontend-port) */ }
    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_FA_STOP) + "  " + tr("Parar", "Stop")).c_str())) { /* TODO(frontend-port) */ }
    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_FA_PAUSE) + "  " + tr("Pausar", "Pause")).c_str())) { /* TODO(frontend-port) */ }
    ImGui::SliderFloat(tr("Tempo", "Time"), &m_animTime, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Velocidade", "Speed"), &m_animSpeed, 0.0f, 2.0f);
    ImGui::End();
}

void WickedToolsPanel::draw_armature_window() {
    if (!showArmatureWindow) return;
    ImGui::Begin(tr("Esqueleto (Armature)", "Armature"), &showArmatureWindow);
    clamp_floating_window_on_screen();
    todo_badge("Armature: listagem de ossos e rig no editor são TODO(frontend-port).",
               "Armature: bone listing and rigging in the editor are TODO(frontend-port).");
    ImGui::TextWrapped("%s", tr(
        "Os esqueletos vivem nos assets de mesh esqueleto (SkeletonAsset). "
        "TODO(frontend-port): listar ossos do asset selecionado, reset de pose "
        "e criação de humanoid rig.",
        "Skeletons live in skinned mesh assets (SkeletonAsset). "
        "TODO(frontend-port): list bones of the selected asset, pose reset and "
        "humanoid rig creation."));
    if (ImGui::Button(tr("Resetar Pose", "Reset Pose"))) { /* TODO(frontend-port) */ }
    ImGui::TextDisabled("%s", tr("(sem osso selecionado)", "(no bone selected)"));
    ImGui::End();
}

void WickedToolsPanel::draw_humanoid_window() {
    if (!showHumanoidWindow) return;
    ImGui::Begin(tr("Humanoide (Humanoid)", "Humanoid Rig"), &showHumanoidWindow);
    clamp_floating_window_on_screen();
    todo_badge("Humanoid: editor de mapeamento de ossos é TODO(frontend-port).",
               "Humanoid: bone-mapping editor is TODO(frontend-port).");
    ImGui::TextWrapped("%s", tr(
        "A engine define HumanoidRigDefinition (mapeamento de ossos) e "
        "AnimationRetargeter. TODO(frontend-port): editor de mapeamento "
        "(osso origem → osso alvo) usando o rig definition.",
        "The engine defines HumanoidRigDefinition (bone mapping) and "
        "AnimationRetargeter. TODO(frontend-port): mapping editor (source bone "
        "→ target bone) using the rig definition."));
    if (ImGui::Button(tr("Criar Rig Humanoide", "Create Humanoid Rig"))) { /* TODO(frontend-port) */ }
    ImGui::End();
}

void WickedToolsPanel::draw_ik_window() {
    if (!showIKWindow) return;
    ImGui::Begin(tr("IK (Cinemática Inversa)", "Inverse Kinematics"), &showIKWindow);
    clamp_floating_window_on_screen();
    todo_badge("IK: alvo do solver no viewport é TODO(frontend-port).",
               "IK: solver target in the viewport is TODO(frontend-port).");
    ImGui::TextWrapped("%s", tr(
        "A engine tem IKSolver (src/engine/animation). TODO(frontend-port): "
        "alvo do solver no viewport (gizmo) e controles de cadeia.",
        "The engine has IKSolver (src/engine/animation). TODO(frontend-port): "
        "solver target in the viewport (gizmo) and chain controls."));
    ImGui::SliderFloat(tr("Força", "Strength"), &m_ikStrength, 0.0f, 1.0f);
    ImGui::End();
}

void WickedToolsPanel::draw_expression_window() {
    if (!showExpressionWindow) return;
    ImGui::Begin(tr("Expressões Faciais", "Expressions"), &showExpressionWindow);
    clamp_floating_window_on_screen();
    todo_badge("Expressions: morph targets/blend shapes são TODO(frontend-port).",
               "Expressions: morph targets/blend shapes are TODO(frontend-port).");
    ImGui::TextWrapped("%s", tr(
        "TODO(frontend-port): morph targets das expressões (blend shapes) e "
        "editor de poses faciais.",
        "TODO(frontend-port): expression morph targets (blend shapes) and a "
        "facial pose editor."));
    ImGui::End();
}

// ===========================================================================
// World / asset / editor windows.
// ===========================================================================

void WickedToolsPanel::draw_terrain_window() {
    if (!showTerrainWindow) return;
    ImGui::Begin(tr("Terreno", "Terrain"), &showTerrainWindow);
    clamp_floating_window_on_screen();
    ImGui::TextWrapped("%s", tr(
        "Gera a mesh de heightmap (ruído value/octaves) como chão da cena. "
        "TODO(frontend-port): colisão do terreno (malha de triângulos estática "
        "não é suportada pelo solver do runtime).",
        "Generates the heightmap mesh (value noise/octaves) as the scene ground. "
        "TODO(frontend-port): terrain collision (static triangle mesh is not "
        "supported by the runtime solver)."));
    ImGui::SliderFloat(tr("Escala", "Scale"), &m_terrainScale, 0.01f, 100.0f);
    ImGui::SliderInt(tr("Oitavas", "Octaves"), &m_terrainOctaves, 1, 8);
    ImGui::SliderFloat(tr("Força (Amount)", "Amount"), &m_terrainAmount, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Suavidade (Falloff)", "Falloff"), &m_terrainFalloff, 0.0f, 1.0f);
    if (ImGui::Button(tr("Gerar Terreno", "Generate Terrain")) && m_applyTerrain) {
        m_applyTerrain(m_terrainScale, m_terrainOctaves, m_terrainAmount, m_terrainFalloff);
        m_terrainStatus = tr("Terreno gerado.", "Terrain generated.");
    }
    if (!m_terrainStatus.empty()) ImGui::TextWrapped("%s", m_terrainStatus.c_str());
    ImGui::End();
}

void WickedToolsPanel::draw_paint_tool_window() {
    if (!showPaintToolWindow) return;
    ImGui::Begin(tr("Ferramenta de Pintura", "Paint Tool"), &showPaintToolWindow);
    clamp_floating_window_on_screen();
    todo_badge("Paint Tool: pintura de vértices/textura em mesh é TODO(frontend-port).",
               "Paint Tool: vertex/texture painting on meshes is TODO(frontend-port).");
    ImGui::TextWrapped("%s", tr(
        "TODO(frontend-port): pintura de vértices/textura em mesh no viewport. "
        "O painel de Escultura de Blocos já cobre pintura voxel (nosso).",
        "TODO(frontend-port): vertex/texture painting on meshes in the viewport. "
        "The Voxel Sculpting panel already covers voxel painting (ours)."));
    ImGui::SliderFloat(tr("Raio do Pincel", "Brush Radius"), &m_paintRadius, 0.01f, 10.0f);
    ImGui::SliderFloat(tr("Força", "Amount"), &m_paintAmount, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("Suavidade", "Smoothness"), &m_paintSmooth, 0.0f, 1.0f);
    ImGui::ColorEdit3(tr("Cor", "Color"), &m_paintColor.r);
    ImGui::End();
}

void WickedToolsPanel::draw_mesh_window() {
    if (!showMeshWindow) return;
    ImGui::Begin(tr("Malha (Mesh)", "Mesh"), &showMeshWindow);
    clamp_floating_window_on_screen();
    ImGui::TextWrapped("%s", tr(
        "Edita as normais da malha do objeto selecionado: recalcula as normais "
        "suaves (por vértice) ou inverte as existentes. O resultado é reenviado "
        "à GPU e reescrito no asset cookado.",
        "Edits the normals of the selected object's mesh: recomputes smooth "
        "(per-vertex) normals or flips the existing ones. The result is "
        "re-uploaded to the GPU and rewritten to the cooked asset."));
    if (ImGui::Button(tr("Recalcular Normais (Suave)", "Compute Normals (Smooth)")) && m_applyMesh) {
        m_meshStatus = m_applyMesh(0);
    }
    if (ImGui::Button(tr("Inverter Normais", "Flip Normals")) && m_applyMesh) {
        m_meshStatus = m_applyMesh(1);
    }
    ImGui::Checkbox(tr("Dupla Face", "Double Sided"), &m_meshDoubleSided);
    if (!m_meshStatus.empty()) ImGui::TextWrapped("%s", m_meshStatus.c_str());
    ImGui::End();
}

void WickedToolsPanel::draw_model_importer_window() {
    if (!showModelImporterWindow) return;
    ImGui::Begin(tr("Importador de Modelos", "Model Importer"), &showModelImporterWindow);
    clamp_floating_window_on_screen();
    ImGui::TextWrapped("%s", tr(
        "Importa glTF/FBX/OBJ/PLY para o registro de assets usando o mesmo "
        "AssetPipeline do Content Browser.",
        "Imports glTF/FBX/OBJ/PLY into the asset registry using the same "
        "AssetPipeline as the Content Browser."));
    static char pathBuf[1024]{};
    ImGui::InputText(tr("Arquivo (.gltf/.fbx/.obj/.ply)", "File (.gltf/.fbx/.obj/.ply)"), pathBuf, sizeof(pathBuf));
    ImGui::SameLine();
    if (ImGui::Button(tr("Procurar...", "Browse..."))) {
        if (m_importAsset) {
            // Ask the editor to run its Windows file dialog (the panel has no
            // window handle); an empty result means the dialog was cancelled.
            const std::string picked = m_importAsset("");
            if (!picked.empty() && picked[0] != '\0') {
                strncpy(pathBuf, picked.c_str(), sizeof(pathBuf) - 1);
                pathBuf[sizeof(pathBuf) - 1] = '\0';
            }
        }
    }
    ImGui::Spacing();
    if (ImGui::Button(tr("Importar", "Import"), ImVec2(0, 0)) && m_importAsset) {
        if (pathBuf[0] == '\0') {
            m_importerStatus = tr("Escolha um arquivo primeiro.", "Pick a file first.");
        } else {
            m_importerStatus = m_importAsset(pathBuf);
        }
    }
    if (!m_importerStatus.empty()) {
        ImGui::TextWrapped("%s", m_importerStatus.c_str());
    }
    ImGui::End();
}

void WickedToolsPanel::draw_video_window() {
    if (!showVideoWindow) return;
    ImGui::Begin(tr("Vídeo", "Video"), &showVideoWindow);
    clamp_floating_window_on_screen();
    todo_badge("Video: player de vídeo (textura dinâmica) é TODO(frontend-port).",
               "Video: video player (dynamic texture) is TODO(frontend-port).");
    ImGui::TextWrapped("%s", tr(
        "TODO(frontend-port): player de vídeo (textura dinâmica) e "
        "decodificação — não há decodificador de vídeo no core ainda.",
        "TODO(frontend-port): video player (dynamic texture) and decoding — no "
        "video decoder exists in the core yet."));
    if (ImGui::Button(tr("Abrir Vídeo...", "Open Video..."))) { /* TODO(frontend-port) */ }
    ImGui::Checkbox(tr("Em Loop", "Looped"), &m_videoLoop);
    ImGui::End();
}

void WickedToolsPanel::draw_gaussian_splat_window() {
    if (!showGaussianSplatWindow) return;
    ImGui::Begin(tr("Gaussian Splat", "Gaussian Splat"), &showGaussianSplatWindow);
    clamp_floating_window_on_screen();
    todo_badge("Gaussian Splat: render de nuvens de splat é TODO(frontend-port).",
               "Gaussian Splat: splat cloud rendering is TODO(frontend-port).");
    ImGui::TextWrapped("%s", tr(
        "TODO(frontend-port): render de nuvens de Gaussian Splatting — sem "
        "suporte no renderer ainda.",
        "TODO(frontend-port): Gaussian Splatting cloud rendering — not supported "
        "by the renderer yet."));
    if (ImGui::Button(tr("Importar .splat...", "Import .splat..."))) { /* TODO(frontend-port) */ }
    ImGui::End();
}

void WickedToolsPanel::draw_theme_editor_window() {
    if (!showThemeEditorWindow) return;
    ImGui::Begin(tr("Editor de Tema", "Theme Editor"), &showThemeEditorWindow);
    clamp_floating_window_on_screen();
    ImGui::TextWrapped("%s", tr(
        "Ajusta as cores base do editor ao vivo (o tema charcoal do Wicked "
        "continua sendo o padrão). TODO(frontend-port): persistir o tema em disco.",
        "Tunes the editor base colors live (the Wicked charcoal theme remains "
        "the default). TODO(frontend-port): persist the theme to disk."));
    ImGui::ColorEdit3(tr("Fundo", "Background"), &m_themeBg.r);
    ImGui::ColorEdit3(tr("Painel", "Panel"), &m_themePanel.r);
    if (ImGui::Button(tr("Aplicar Tema", "Apply Theme"))) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_WindowBg] = ImVec4(m_themeBg.r, m_themeBg.g, m_themeBg.b, 1.0f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(m_themePanel.r, m_themePanel.g, m_themePanel.b, 1.0f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(m_themePanel.r, m_themePanel.g, m_themePanel.b, 1.0f);
        style.Colors[ImGuiCol_MenuBarBg] = ImVec4(m_themePanel.r, m_themePanel.g, m_themePanel.b, 1.0f);
        // Derive the frame/button tones from the panel color (slightly lighter).
        const float lift = 0.08f;
        style.Colors[ImGuiCol_FrameBg] = ImVec4(m_themePanel.r + lift, m_themePanel.g + lift, m_themePanel.b + lift, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(m_themePanel.r + lift, m_themePanel.g + lift, m_themePanel.b + lift, 1.0f);
    }
    ImGui::End();
}

void WickedToolsPanel::draw_project_creator_window() {
    if (!showProjectCreatorWindow) return;
    ImGui::Begin(tr("Criador de Projetos", "Project Creator"), &showProjectCreatorWindow);
    clamp_floating_window_on_screen();
    static char nameBuf[128] = "MeuJogo";
    static char dirBuf[512];
    ImGui::InputText(tr("Nome do Projeto", "Project Name"), nameBuf, sizeof(nameBuf));
    ImGui::InputText(tr("Pasta", "Folder"), dirBuf, sizeof(dirBuf));
    ImGui::TextWrapped("%s", tr(
        "Cria a pasta do projeto com cena vazia e registro de assets zerado; o "
        "Gerenciador de Jogos lista projetos a partir de Projects/.",
        "Creates the project folder with an empty scene and empty asset registry; "
        "the Game Launcher lists projects from Projects/."));
    if (ImGui::Button(tr("Criar Projeto", "Create Project")) && m_createProject) {
        const std::string name = nameBuf[0] != '\0' ? std::string(nameBuf) : "MeuJogo";
        m_projectStatus = m_createProject(name, dirBuf);
    }
    if (!m_projectStatus.empty()) ImGui::TextWrapped("%s", m_projectStatus.c_str());
    ImGui::End();
}

void WickedToolsPanel::draw_general_window() {
    if (!showGeneralWindow) return;
    ImGui::Begin(tr("Opções Gerais", "General Options"), &showGeneralWindow);
    clamp_floating_window_on_screen();
    ImGui::TextWrapped("%s", tr(
        "Preferências do editor persistidas em settings.json (idioma, tema, VSync, sombras).",
        "Editor preferences persisted to settings.json (language, theme, VSync, shadows)."));
    if (m_language) {
        // Toggle Português ⇄ English through the opaque enum pointer.
        bool pt = is_pt();
        if (ImGui::Checkbox(tr("Idioma: Português", "Language: Português"), &pt)) {
            int* lang = static_cast<int*>(m_language);
            *lang = pt ? 0 : 1; // EngineLanguage::PT_BR=0, EN_US=1
        }
    }
    if (ImGui::Button(tr("Salvar Configurações", "Save Settings")) && m_saveSettings) m_saveSettings();
    ImGui::End();
}

void WickedToolsPanel::draw_graphics_window() {
    if (!showGraphicsWindow) return;
    ImGui::Begin(tr("Opções Gráficas", "Graphics Options"), &showGraphicsWindow);
    clamp_floating_window_on_screen();
    ImGui::TextWrapped("%s", tr(
        "Aplica o VSync ao swapchain (FIFO/MAILBOX) e a resolução do mapa de "
        "sombras (512/1024/2048/4096).",
        "Applies VSync to the swapchain (FIFO/MAILBOX) and the shadow map "
        "resolution (512/1024/2048/4096)."));
    ImGui::Checkbox(tr("VSync", "VSync"), &m_gfxVSync);
    ImGui::SliderInt(tr("Qualidade de Sombras", "Shadow Quality"), &m_gfxShadowQuality, 1, 4);
    if (ImGui::Button(tr("Aplicar", "Apply")) && m_applyGraphics) {
        m_applyGraphics(m_gfxVSync, m_gfxShadowQuality);
        m_graphicsStatus = tr("Aplicado.", "Applied.");
    }
    if (!m_graphicsStatus.empty()) ImGui::TextWrapped("%s", m_graphicsStatus.c_str());
    ImGui::End();
}

void WickedToolsPanel::draw_profiler_window() {
    if (!showProfilerWindow) return;
    ImGui::Begin(tr("Profiler", "Profiler"), &showProfilerWindow);
    clamp_floating_window_on_screen();
    ImGui::TextWrapped("%s", tr(
        "Gráfico de tempo de frame dos últimos segundos, alimentado a cada frame.",
        "Frame-time graph of the last seconds, fed every frame."));
    if (!m_frameTimes.empty()) {
        ImGui::PlotLines(tr("Tempo de Frame (ms)", "Frame Time (ms)"),
                         m_frameTimes.data(), static_cast<int>(m_frameTimes.size()),
                         0, nullptr, 0.0f, 33.0f,
                         ImVec2(ImGui::GetContentRegionAvail().x, 140));
    }
    ImGui::Text(tr("FPS: %.1f   Frame: %.2f ms", "FPS: %.1f   Frame: %.2f ms"), m_statFps, m_statFrameMs);
    ImGui::End();
}

void WickedToolsPanel::todo_badge(const char* pt, const char* en) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", tr("⚠ Sem runtime ainda: o que você autorar aqui é salvo na cena, mas nada é simulado/renderizado no momento.", "⚠ No runtime yet: what you author here is saved to the scene, but nothing simulates/renders it yet."));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr(pt, en));
    }
    ImGui::Separator();
}

void WickedToolsPanel::draw_dev_window() {
    if (!showDevWindow) return;
    ImGui::Begin(tr("Painel de Desenvolvimento", "Developer Panel"), &showDevWindow);
    clamp_floating_window_on_screen();

    const auto statusLine = [&](const char* labelPt, const char* labelEn, bool ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ok ? ImVec4(0.40f, 0.85f, 0.45f, 1.0f)
                                                : ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
        ImGui::BulletText("%s", tr(labelPt, labelEn));
        ImGui::PopStyleColor();
    };

    // ------------------------------------------------------------------
    // Control API — the same commands the HTTP API (127.0.0.1:8321) accepts,
    // now with buttons instead of curl.
    // ------------------------------------------------------------------
    if (ImGui::CollapsingHeader(tr("Controle (Control API)", "Control (Control API)"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool isEdit = m_playState == 0;
        const bool isPlay = m_playState == 1;
        const bool isPause = m_playState == 2;
        const bool notEdit = !isEdit;
        if (m_controlCmd) {
            if (ImGui::Button(tr("▶ PLAY", "▶ PLAY"), ImVec2(-FLT_MIN, 28)) && isEdit) m_controlCmd("play");
            ImGui::BeginDisabled(!isPlay);
            if (ImGui::Button(tr("⏸ PAUSAR", "⏸ PAUSE"), ImVec2(-FLT_MIN, 24))) m_controlCmd("pause");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!isPause);
            if (ImGui::Button(tr("▶ CONTINUAR", "▶ RESUME"), ImVec2(-FLT_MIN, 24))) m_controlCmd("resume");
            if (ImGui::Button(tr("PASSO (1 frame)", "STEP (1 frame)"), ImVec2(-FLT_MIN, 24))) m_controlCmd("step");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!notEdit);
            if (ImGui::Button(tr("⏹ PARAR", "⏹ STOP"), ImVec2(-FLT_MIN, 24))) m_controlCmd("stop");
            ImGui::EndDisabled();
        } else {
            ImGui::TextDisabled("%s", tr("(callback não ligado)", "(callback not wired)"));
        }
        ImGui::Separator();
        static float zoomAmt = 0.1f;
        ImGui::SliderFloat(tr("Zoom", "Zoom"), &zoomAmt, -0.5f, 0.5f);
        if (ImGui::Button(tr("Aplicar Zoom", "Apply Zoom"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) {
            m_controlCmd("zoom " + std::to_string(zoomAmt));
        }
        if (ImGui::Button(tr("Câmera: Frente", "Camera: Forward"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) m_controlCmd("move 1 0 0");
        if (ImGui::Button(tr("Câmera: Trás", "Camera: Back"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) m_controlCmd("move -1 0 0");
        if (ImGui::Button(tr("Câmera: Esquerda", "Camera: Left"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) m_controlCmd("move 0 -1 0");
        if (ImGui::Button(tr("Câmera: Direita", "Camera: Right"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) m_controlCmd("move 0 1 0");
        if (ImGui::Button(tr("Câmera: Subir", "Camera: Up"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) m_controlCmd("move 0 0 1");
        if (ImGui::Button(tr("Câmera: Descer", "Camera: Down"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) m_controlCmd("move 0 0 -1");
        ImGui::Separator();
        if (ImGui::Button(tr("Gerar Terreno (sliders da janela Terreno)", "Generate Terrain (Terrain window sliders)"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) {
            m_controlCmd("terrain " + std::to_string(m_terrainScale) + " " +
                         std::to_string(m_terrainOctaves) + " " +
                         std::to_string(m_terrainAmount) + " " +
                         std::to_string(m_terrainFalloff));
        }
        if (ImGui::Button(tr("Aplicar Gráficas (VSync/sombras)", "Apply Graphics (VSync/shadows)"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) {
            m_controlCmd(std::string("graphics ") + (m_gfxVSync ? "1" : "0") + " " +
                         std::to_string(m_gfxShadowQuality));
        }
        if (ImGui::Button(tr("Salvar Configurações", "Save Settings"), ImVec2(-FLT_MIN, 0)) && m_controlCmd) m_controlCmd("save-settings");
        ImGui::Separator();
        ImGui::TextWrapped("%s", tr(
            "Equivalente via curl:  curl http://127.0.0.1:8321/cmd?cmd=play\n"
            "(step, pause, resume, stop, zoom 0.1, move 1 0 0, turn 45 0, "
            "terrain 1 4 0.5 0.4, graphics 1 3, save-settings, project Nome, "
            "mesh 0).",
            "Equivalent via curl:  curl http://127.0.0.1:8321/cmd?cmd=play\n"
            "(step, pause, resume, stop, zoom 0.1, move 1 0 0, turn 45 0, "
            "terrain 1 4 0.5 0.4, graphics 1 3, save-settings, project Name, "
            "mesh 0)."));
    }

    // ------------------------------------------------------------------
    // Self-tests — spawn the editor headless with the test env var and show
    // PASS/FAIL from the exit code.
    // ------------------------------------------------------------------
    if (ImGui::CollapsingHeader(tr("Self-Tests (headless)", "Self-Tests (headless)"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_selfTest) {
            const char* tests[5] = { "Render Graph", "HDR Texture", "Material Graph", "Play / Physics", "Build Game" };
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Button(tests[i], ImVec2(-FLT_MIN, 24))) {
                    m_devStatus = std::string(tests[i]) + ": " + m_selfTest(i);
                }
            }
        } else {
            ImGui::TextDisabled("%s", tr("(callback não ligado)", "(callback not wired)"));
        }
        if (!m_devStatus.empty()) {
            ImGui::TextWrapped("%s", m_devStatus.c_str());
        }
        ImGui::TextWrapped("%s", tr(
            "Cada teste abre uma segunda instância do editor em modo headless "
            "com a variável de ambiente do teste e sai com PASS/FAIL.",
            "Each test spawns a second headless editor instance with the test "
            "environment variable and exits with PASS/FAIL."));
    }

    // ------------------------------------------------------------------
    // Packaging & services.
    // ------------------------------------------------------------------
    if (ImGui::CollapsingHeader(tr("Empacotamento & Serviços", "Packaging & Services"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button(tr("Empacotar Assets (standalone)", "Package Assets (standalone)"), ImVec2(-FLT_MIN, 28)) && m_packageAssets) {
            m_packageStatus = m_packageAssets();
        }
        if (!m_packageStatus.empty()) ImGui::TextWrapped("%s", m_packageStatus.c_str());
        ImGui::TextWrapped("%s", tr(
            "Empacota os assets cozidos em Intermediate/Package (mesmo "
            "AssetPackager do Build, sem cozinhar nem gerar o jogo). O Build "
            "completo continua disponível no botão Build da barra.",
            "Packages cooked assets into Intermediate/Package (the same "
            "AssetPackager the Build uses, without cooking or producing the "
            "game). The full Build stays available from the toolbar button."));
        ImGui::Separator();
        if (m_hotReloadStatus) {
            ImGui::Text("%s", tr("Reimportação automática (hot reload):", "Automatic reimport (hot reload):"));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", m_hotReloadStatus().c_str());
        }
        ImGui::TextWrapped("%s", tr(
            "Vigia os arquivos de origem e reimporta os que mudarem no disco "
            "(rodando em segundo plano, sem janela própria).",
            "Watches source files and reimports the ones that change on disk "
            "(runs in the background, with no dedicated window)."));
    }

    // ------------------------------------------------------------------
    // Honest status — what actually works vs. authored-but-inert systems.
    // ------------------------------------------------------------------
    if (ImGui::CollapsingHeader(tr("Status Honesto dos Sistemas", "Honest System Status"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", tr(
            "Verde = funciona de ponta a ponta. Vermelho = os dados são "
            "autorados e salvos na cena, mas o runtime ainda não simula/renderiza.",
            "Green = works end to end. Red = data is authored and saved to the "
            "scene, but the runtime does not simulate/render it yet."));
        ImGui::Separator();
        statusLine("Física, armas, partículas, veículos, ragdolls, missões, diálogos, áudio espacial, navegação", "Physics, weapons, particles, vehicles, ragdolls, missions, dialogue, spatial audio, navigation", true);
        statusLine("Force fields, molas, vínculos, splines, clima (simulados no play)", "Force fields, springs, constraints, splines, weather (simulated in play)", true);
        statusLine("Voxel: volume, brush e pintura no viewport (painel Escultura de Blocos)", "Voxel: volume, brush and viewport painting (Voxel Sculpting panel)", true);
        statusLine("Assets: thumbnails reais, preview de áudio, blocos estilo Minecraft, double-click", "Assets: real thumbnails, audio preview, Minecraft-style blocks, double-click", true);
        statusLine("Scripts: VM, hot reload, debugger, canvas visual", "Scripts: VM, hot reload, debugger, visual canvas", true);
        statusLine("Camadas (Layers) — runtime de camadas é TODO", "Layers — layer runtime is TODO", false);
        statusLine("Colisor — a forma autorada não define a shape no play (usa default)", "Collider — the authored shape does not define the play body (uses default)", false);
        statusLine("Terreno — mesh visual sem colisão (solver sem mesh estática)", "Terrain — visual mesh without collision (no static-mesh solver)", false);
        statusLine("Decal, Hair (fios), EnvProbe, SoftBody — sem pass no renderer/solver", "Decal, Hair, EnvProbe, SoftBody — no renderer pass/solver", false);
        statusLine("Animação (playback de clips), Armature, Humanoid, IK, Expressões — TODO no play", "Animation (clip playback), Armature, Humanoid, IK, Expressions — TODO in play", false);
        statusLine("Pintura de mesh, Vídeo, Gaussian Splat — painéis TODO explícito", "Mesh painting, Video, Gaussian Splat — explicit TODO panels", false);
        statusLine("Editores Animation/Timeline/IK/Retarget — documentos com undo/redo, sem Apply à cena", "Animation/Timeline/IK/Retarget editors — undo/redo documents, no scene Apply", false);
        statusLine("EditorGUI legado — camada duplicada desativada, só guarda seleção", "Legacy EditorGUI — disabled duplicate layer, only keeps selection", false);
    }

    ImGui::End();
}

void WickedToolsPanel::draw_guide_window() {
    if (!showGuideWindow) return;
    ImGui::Begin(tr("Como Usar", "How to Use"), &showGuideWindow);
    clamp_floating_window_on_screen();
    ImGui::BeginChild("##guideBody", ImVec2(0, 0), true);
    ImGui::TextWrapped("%s", tr(
        "ATALHOS\n"
        "  Q  Selecionar (sem gizmo)   W/E/R  Mover/Rotacionar/Escalar\n"
        "  Ctrl + arrastar  snap (passo definido no combo do viewport)\n"
        "  Ctrl+K  Paleta de Comandos   Botão direito na toolbar de play  PARAR\n"
        "\n"
        "CENA\n"
        "  +Add (painel Cena) cria entidades; arraste na árvore para reparentar;\n"
        "  Delete remove; a busca filtra; o Inspector edita componentes e o \"+ Add\n"
        "  Component\" pesquisa por nome. Menu ⋯ do viewport liga grid/gizmos/colisores.\n"
        "\n"
        "PLAY\n"
        "  PLAY testa o jogo no editor (física, armas com ESPAÇO, veículos com setas).\n"
        "  PAUSAR congela; PASSO (na toolbar e no Painel de Desenvolvimento) avança\n"
        "  1 frame; botão direito do mouse no PLAY ou PARAR volta ao modo edição.\n"
        "\n"
        "ASSETS\n"
        "  Importar (barra) cozinha PNG/JPG/HDR/WAV/OGG/glTF/FBX/OBJ/PLY.\n"
        "  Texturas quadradas POT 8-256px são reconhecidas como face de bloco e\n"
        "  aparecem na aba Modelos; clique direito em qualquer textura → \"Criar\n"
        "  Modelo de Bloco\" para montar o cubo (preview 3D real).\n"
        "  ▶ no card de .wav reproduz; ▶ em outro para o anterior (voz única).\n"
        "  Duplo clique num asset abre o editor especializado (Material, Áudio...).\n"
        "  Arraste malha/material para o viewport para instanciar no objeto.\n"
        "\n"
        "VOXEL (ESCULTURA DE BLOCOS)\n"
        "  +Add → \"Mundo de Blocos\" (Voxel World); selecione a entidade; no painel\n"
        "  Escultura de Blocos marque \"Pintar no viewport\" e arraste sobre o volume:\n"
        "  esquerdo coloca, direito remove. \"Gerar Terreno\" cria relevo pela semente;\n"
        "  os blocos são renderizados no editor e no play.\n"
        "\n"
        "FERRAMENTAS\n"
        "  Menu Ferramentas (ou barra): Objeto, Luz, Câmera, Material, Som, Corpo\n"
        "  Rígido, Colisor, Emissor, Curva, Campo de Força, Clima, Terreno, Profiler.\n"
        "  Editores especializados (Ragdoll, Weapon, Vehicle, ...) aplicam à cena\n"
        "  com \"Apply to Selected\". Janelas com aviso vermelho ⚠ ainda não têm\n"
        "  runtime — o Painel de Desenvolvimento lista o que funciona e o que não.\n"
        "\n"
        "DESENVOLVEDOR\n"
        "  Control API HTTP em 127.0.0.1:8321 (play/step/zoom/move/terrain/...).\n"
        "  Self-tests headless: Painel de Desenvolvimento → Self-Tests.\n"
        "  Detalhes do port: src/editor/frontend/PORTS.md.",
        "SHORTCUTS\n"
        "  Q  Select (no gizmo)   W/E/R  Move/Rotate/Scale\n"
        "  Ctrl + drag  snap (step from the viewport combo)\n"
        "  Ctrl+K  Command Palette   Right-click the play button  STOP\n"
        "\n"
        "SCENE\n"
        "  +Add (Scene panel) creates entities; drag in the tree to reparent;\n"
        "  Delete removes; the search filters; the Inspector edits components and\n"
        "  \"+ Add Component\" searches by name. Viewport ⋯ menu toggles grid/gizmos/colliders.\n"
        "\n"
        "PLAY\n"
        "  PLAY runs the game in-editor (physics, SPACE weapons, arrow-key vehicles).\n"
        "  PAUSE freezes; STEP (toolbar and Developer Panel) advances 1 frame;\n"
        "  right-clicking PLAY or STOP returns to edit mode.\n"
        "\n"
        "ASSETS\n"
        "  Import (toolbar) cooks PNG/JPG/HDR/WAV/OGG/glTF/FBX/OBJ/PLY.\n"
        "  Square POT 8-256px textures are recognized as block faces and show up\n"
        "  in the Models tab; right-click any texture → \"Create Block Model\" to\n"
        "  build the cube (real 3D preview).\n"
        "  ▶ on a .wav card plays it; ▶ on another stops the previous (single voice).\n"
        "  Double-click an asset opens its specialized editor (Material, Audio...).\n"
        "  Drag a mesh/material onto the viewport to instantiate on the object.\n"
        "\n"
        "VOXEL (SCULPTING)\n"
        "  +Add → \"Voxel World\"; select it; in the Voxel Sculpting panel check\n"
        "  \"Paint in viewport\" and drag over the volume: left adds, right removes.\n"
        "  \"Generate Terrain\" builds relief from the seed; blocks render in the\n"
        "  editor and in play.\n"
        "\n"
        "TOOLS\n"
        "  Tools menu (or toolbar): Object, Light, Camera, Material, Sound, Rigid\n"
        "  Body, Collider, Emitter, Spline, Force Field, Weather, Terrain, Profiler.\n"
        "  Specialized editors (Ragdoll, Weapon, Vehicle, ...) apply to the scene\n"
        "  with \"Apply to Selected\". Windows with the red ⚠ badge have no runtime\n"
        "  yet — the Developer Panel lists what works and what does not.\n"
        "\n"
        "DEVELOPER\n"
        "  Control HTTP API at 127.0.0.1:8321 (play/step/zoom/move/terrain/...).\n"
        "  Headless self-tests: Developer Panel → Self-Tests.\n"
        "  Port details: src/editor/frontend/PORTS.md."));
    ImGui::EndChild();
    ImGui::End();
}

void WickedToolsPanel::draw_tools_menu() {
    if (ImGui::BeginMenu(tr("Ferramentas", "Tools"))) {
        ImGui::MenuItem(tr("Nome do Objeto", "Object Name"), nullptr, &showNameWindow);
        ImGui::MenuItem(tr("Camadas", "Layers"), nullptr, &showLayerWindow);
        ImGui::MenuItem(tr("Objeto", "Object"), nullptr, &showObjectWindow);
        ImGui::Separator();
        ImGui::MenuItem(tr("Luz", "Light"), nullptr, &showLightWindow);
        ImGui::MenuItem(tr("Câmera", "Camera"), nullptr, &showCameraWindow);
        ImGui::MenuItem(tr("Material", "Material"), nullptr, &showMaterialWindow);
        ImGui::MenuItem(tr("Som", "Sound"), nullptr, &showSoundWindow);
        ImGui::Separator();
        ImGui::MenuItem(tr("Corpo Rígido", "Rigid Body"), nullptr, &showRigidBodyWindow);
        ImGui::MenuItem(tr("Colisor", "Collider"), nullptr, &showColliderWindow);
        ImGui::MenuItem(tr("Vínculo (Constraint)", "Constraint"), nullptr, &showConstraintWindow);
        ImGui::MenuItem(tr("Corpo Mole (Soft Body)", "Soft Body"), nullptr, &showSoftBodyWindow);
        ImGui::MenuItem(tr("Mola (Spring)", "Spring"), nullptr, &showSpringWindow);
        ImGui::Separator();
        ImGui::MenuItem(tr("Animação", "Animation"), nullptr, &showAnimationWindow);
        ImGui::MenuItem(tr("Esqueleto (Armature)", "Armature"), nullptr, &showArmatureWindow);
        ImGui::MenuItem(tr("Humanoide (Humanoid)", "Humanoid Rig"), nullptr, &showHumanoidWindow);
        ImGui::MenuItem(tr("IK", "IK"), nullptr, &showIKWindow);
        ImGui::MenuItem(tr("Expressões Faciais", "Expressions"), nullptr, &showExpressionWindow);
        ImGui::Separator();
        ImGui::MenuItem(tr("Emissor de Partículas", "Particle Emitter"), nullptr, &showEmitterWindow);
        ImGui::MenuItem(tr("Cabelo / Fios (Hair)", "Hair Particles"), nullptr, &showHairParticleWindow);
        ImGui::MenuItem(tr("Curva (Spline)", "Spline"), nullptr, &showSplineWindow);
        ImGui::MenuItem(tr("Campo de Força", "Force Field"), nullptr, &showForceFieldWindow);
        ImGui::MenuItem(tr("Decalque (Decal)", "Decal"), nullptr, &showDecalWindow);
        ImGui::MenuItem(tr("Sonda Ambiental (Env Probe)", "Environment Probe"), nullptr, &showEnvProbeWindow);
        ImGui::MenuItem(tr("Clima e Céu", "Weather & Sky"), nullptr, &showWeatherWindow);
        ImGui::Separator();
        ImGui::MenuItem(tr("Terreno", "Terrain"), nullptr, &showTerrainWindow);
        ImGui::MenuItem(tr("Ferramenta de Pintura", "Paint Tool"), nullptr, &showPaintToolWindow);
        ImGui::MenuItem(tr("Malha (Mesh)", "Mesh"), nullptr, &showMeshWindow);
        ImGui::MenuItem(tr("Importador de Modelos", "Model Importer"), nullptr, &showModelImporterWindow);
        ImGui::MenuItem(tr("Vídeo", "Video"), nullptr, &showVideoWindow);
        ImGui::MenuItem(tr("Gaussian Splat", "Gaussian Splat"), nullptr, &showGaussianSplatWindow);
        ImGui::Separator();
        ImGui::MenuItem(tr("Editor de Tema", "Theme Editor"), nullptr, &showThemeEditorWindow);
        ImGui::MenuItem(tr("Criador de Projetos", "Project Creator"), nullptr, &showProjectCreatorWindow);
        ImGui::MenuItem(tr("Opções Gerais", "General Options"), nullptr, &showGeneralWindow);
        ImGui::MenuItem(tr("Opções Gráficas", "Graphics Options"), nullptr, &showGraphicsWindow);
        ImGui::MenuItem(tr("Profiler", "Profiler"), nullptr, &showProfilerWindow);
        ImGui::Separator();
        ImGui::MenuItem(tr("Painel de Desenvolvimento", "Developer Panel"), nullptr, &showDevWindow);
        ImGui::MenuItem(tr("Como Usar (Guia)", "How to Use (Guide)"), nullptr, &showGuideWindow);
        ImGui::EndMenu();
    }
}

void WickedToolsPanel::draw() {
    if (!m_scene) return;
    draw_name_window();
    draw_layer_window();
    draw_object_window();
    draw_light_window();
    draw_camera_window();
    draw_material_window();
    draw_sound_window();
    draw_rigidbody_window();
    draw_collider_window();
    draw_constraint_window();
    draw_softbody_window();
    draw_spring_window();
    draw_decal_window();
    draw_emitter_window();
    draw_hair_particle_window();
    draw_spline_window();
    draw_force_field_window();
    draw_env_probe_window();
    draw_weather_window();
    draw_animation_window();
    draw_armature_window();
    draw_humanoid_window();
    draw_ik_window();
    draw_expression_window();
    draw_terrain_window();
    draw_paint_tool_window();
    draw_mesh_window();
    draw_model_importer_window();
    draw_video_window();
    draw_gaussian_splat_window();
    draw_theme_editor_window();
    draw_project_creator_window();
    draw_general_window();
    draw_graphics_window();
    draw_profiler_window();
    draw_dev_window();
    draw_guide_window();
}

} // namespace Engine
