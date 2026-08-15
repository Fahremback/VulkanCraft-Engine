#include "EditorGUI.hpp"
#include <iostream>
#include <imgui.h>
#include "../../core/reflection/Reflection.hpp"

namespace Engine {

void EditorGUI::init(Scene* scene, UndoSystem* undoSystem) {
    m_activeScene = scene;
    m_undoSystem = undoSystem;
    std::cout << "[EditorGUI] Visual Engine GUI initialized successfully." << std::endl;
}

void EditorGUI::update(float deltaTime) {
    if (!m_activeScene) return;

    // Direct update logic for active scene and PlayModeManager
    Scene* scene = m_activeScene;
    if (scene) m_activeScene = scene;

    draw_menu_bar();
    draw_toolbar();
    if (showOutliner) draw_world_outliner();
    if (showInspector) draw_inspector();
    if (showContentBrowser) draw_content_browser();
#if VC_ENABLE_VOXEL_PLUGIN
    if (showVoxelTools) draw_voxel_editor_panel();
#endif
    if (showConsole) draw_console();
}

void EditorGUI::draw_menu_bar() {
    // Menu bar logic: File (New Scene, Open, Save, Export Build), Edit (Undo, Redo), Window (Outliner, Inspector, Content Browser)
}

void EditorGUI::draw_toolbar() {
    // Toolbar controls: Play, Pause, Stop PIE buttons
}

void EditorGUI::draw_world_outliner() {
    if (!m_activeScene) return;
    if (!ImGui::Begin("World Outliner", &showOutliner)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("+ Create Entity")) {
        if (m_undoSystem) {
            m_undoSystem->execute_command(std::make_unique<CreateEntityCommand>(m_activeScene, "New Entity"));
        } else {
            m_activeScene->create_entity("New Entity");
        }
    }
    ImGui::Separator();

    if (ImGui::BeginPopupContextWindow(nullptr, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Create Entity")) {
            if (m_undoSystem) {
                m_undoSystem->execute_command(std::make_unique<CreateEntityCommand>(m_activeScene, "New Entity"));
            } else {
                m_activeScene->create_entity("New Entity");
            }
        }
        ImGui::EndPopup();
    }

    Entity entityToDelete;
    std::function<void(UUID)> draw_entity_node = [&](UUID id) {
        Entity entity = m_activeScene->find_entity_by_id(id);
        if (!entity.is_valid()) return;

        const std::vector<UUID> children = m_activeScene->get_children(id);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (m_selectedEntity == entity) flags |= ImGuiTreeNodeFlags_Selected;
        if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;

        bool nodeOpen = ImGui::TreeNodeEx((void*)(uintptr_t)id.get_low(), flags, "%s", entity.get_name().c_str());
        if (ImGui::IsItemClicked()) {
            m_selectedEntity = entity;
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete Entity")) {
                entityToDelete = entity;
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("OUTLINER_ENTITY", &id, sizeof(UUID));
            ImGui::Text("Move %s", entity.get_name().c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("OUTLINER_ENTITY")) {
                UUID draggedID = *(const UUID*)payload->Data;
                UUID oldParentID = m_activeScene->get_parent(draggedID);
                if (m_undoSystem) {
                    m_undoSystem->execute_command(std::make_unique<ReparentEntityCommand>(m_activeScene, draggedID, oldParentID, id));
                } else {
                    m_activeScene->set_parent(draggedID, id);
                }
            }
            // Asset drop: material do Content Browser aplicado direto nesta
            // entidade (mesmo caminho do viewport).
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_ASSET_UUID")) {
                if (payload->DataSize > 1) {
                    const std::string droppedId(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                    const UUID assetId = UUID::from_string(droppedId);
                    const auto asset = m_assetRegistry ? m_assetRegistry->find(assetId) : std::optional<AssetMetadata>{};
                    if (asset && asset->type == AssetType::Material) {
                        auto it = m_activeScene->meshRendererComponents.find(id);
                        if (it != m_activeScene->meshRendererComponents.end()) {
                            it->second.materialAssetID = assetId;
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (nodeOpen) {
            for (UUID childID : children) {
                draw_entity_node(childID);
            }
            ImGui::TreePop();
        }
    };

    for (const auto& [id, entity] : m_activeScene->get_entities()) {
        if (!m_activeScene->get_parent(id).is_valid()) {
            draw_entity_node(id);
        }
    }

    if (entityToDelete.is_valid()) {
        if (m_selectedEntity == entityToDelete) {
            m_selectedEntity = Entity{};
        }
        if (m_undoSystem) {
            m_undoSystem->execute_command(std::make_unique<DeleteEntityCommand>(m_activeScene, entityToDelete.get_id()));
        } else {
            m_activeScene->destroy_entity(entityToDelete.get_id());
        }
    }

    ImGui::End();
}

void EditorGUI::draw_inspector() {
    if (!m_selectedEntity.is_valid() || !m_activeScene) return;
    if (!ImGui::Begin("Inspector", &showInspector)) {
        ImGui::End();
        return;
    }

    const UUID id = m_selectedEntity.get_id();
    char nameBuf[256];
    snprintf(nameBuf, sizeof(nameBuf), "%s", m_selectedEntity.get_name().c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string newName(nameBuf);
        if (newName != m_selectedEntity.get_name()) {
            if (m_undoSystem) {
                m_undoSystem->execute_command(std::make_unique<RenameEntityCommand>(
                    m_activeScene, id, m_selectedEntity.get_name(), newName));
            } else {
                m_activeScene->rename_entity(id, newName);
            }
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Add Component");
    if (!m_activeScene->lightComponents.contains(id) && ImGui::Button("LightComponent")) {
        if (m_undoSystem) {
            m_undoSystem->execute_command(std::make_unique<AddComponentCommand>(
                "Add LightComponent",
                [scene = m_activeScene, id] { scene->lightComponents[id] = LightComponent{}; },
                [scene = m_activeScene, id] { scene->lightComponents.erase(id); }));
        }
    }
    ImGui::SameLine();
    if (!m_activeScene->cameraComponents.contains(id) && ImGui::Button("CameraComponent")) {
        if (m_undoSystem) {
            m_undoSystem->execute_command(std::make_unique<AddComponentCommand>(
                "Add CameraComponent",
                [scene = m_activeScene, id] { scene->cameraComponents[id] = CameraComponent{}; },
                [scene = m_activeScene, id] { scene->cameraComponents.erase(id); }));
        }
    }
    if (!m_activeScene->rigidbodyComponents.contains(id) && ImGui::Button("RigidbodyComponent")) {
        if (m_undoSystem) {
            m_undoSystem->execute_command(std::make_unique<AddComponentCommand>(
                "Add RigidbodyComponent",
                [scene = m_activeScene, id] { scene->rigidbodyComponents[id] = RigidbodyComponent{}; },
                [scene = m_activeScene, id] { scene->rigidbodyComponents.erase(id); }));
        }
    }
    ImGui::SameLine();
    if (!m_activeScene->materialComponents.contains(id) && ImGui::Button("MaterialComponent")) {
        if (m_undoSystem) {
            m_undoSystem->execute_command(std::make_unique<AddComponentCommand>(
                "Add MaterialComponent",
                [scene = m_activeScene, id] { scene->materialComponents[id] = MaterialComponent{}; },
                [scene = m_activeScene, id] { scene->materialComponents.erase(id); }));
        }
    }
    ImGui::Separator();

    auto draw_component = [&](const char* componentName, void* component) {
        const ClassMetaData* metadata = TypeRegistry::get().find_class(componentName);
        if (!metadata || !component || !ImGui::TreeNode(componentName)) return;
        for (const auto& field : metadata->fields) {
            std::any value = field.getter(component);
            bool changed = false;
            switch (field.type) {
            case FieldType::Float: {
                float current = std::any_cast<float>(value);
                if (field.metadata.hasRange) {
                    changed = ImGui::SliderFloat(field.metadata.displayName.c_str(), &current,
                        field.metadata.rangeMin, field.metadata.rangeMax);
                } else {
                    changed = ImGui::DragFloat(field.metadata.displayName.c_str(), &current, 0.1f);
                }
                if (changed && m_undoSystem) {
                    const auto setter = field.setter;
                    const std::any oldValue = value;
                    const std::any newValue = current;
                    m_undoSystem->execute_or_merge_property(
                        std::string(componentName) + "." + field.metadata.displayName,
                        [component, setter, newValue] { setter(component, newValue); },
                        [component, setter, oldValue] { setter(component, oldValue); },
                        !ImGui::IsItemActivated());
                }
                break;
            }
            case FieldType::Int: {
                int current = std::any_cast<int>(value);
                if (field.metadata.hasRange) {
                    changed = ImGui::SliderInt(field.metadata.displayName.c_str(), &current,
                        static_cast<int>(field.metadata.rangeMin), static_cast<int>(field.metadata.rangeMax));
                } else {
                    changed = ImGui::DragInt(field.metadata.displayName.c_str(), &current, 1.0f);
                }
                if (changed && m_undoSystem) {
                    const auto setter = field.setter;
                    const std::any oldValue = value;
                    const std::any newValue = current;
                    m_undoSystem->execute_or_merge_property(
                        std::string(componentName) + "." + field.metadata.displayName,
                        [component, setter, newValue] { setter(component, newValue); },
                        [component, setter, oldValue] { setter(component, oldValue); },
                        !ImGui::IsItemActivated());
                }
                break;
            }
            case FieldType::Bool: {
                bool current = std::any_cast<bool>(value);
                changed = ImGui::Checkbox(field.metadata.displayName.c_str(), &current);
                if (changed && m_undoSystem) {
                    const auto setter = field.setter;
                    const std::any oldValue = value;
                    const std::any newValue = current;
                    m_undoSystem->execute_or_merge_property(
                        std::string(componentName) + "." + field.metadata.displayName,
                        [component, setter, newValue] { setter(component, newValue); },
                        [component, setter, oldValue] { setter(component, oldValue); },
                        !ImGui::IsItemActivated());
                }
                break;
            }
            case FieldType::Vec3: {
                glm::vec3 current = std::any_cast<glm::vec3>(value);
                changed = ImGui::DragFloat3(field.metadata.displayName.c_str(), &current.x, 0.1f);
                if (changed && m_undoSystem) {
                    const auto setter = field.setter;
                    const std::any oldValue = value;
                    const std::any newValue = current;
                    m_undoSystem->execute_or_merge_property(
                        std::string(componentName) + "." + field.metadata.displayName,
                        [component, setter, newValue] { setter(component, newValue); },
                        [component, setter, oldValue] { setter(component, oldValue); },
                        !ImGui::IsItemActivated());
                }
                break;
            }
            case FieldType::Color: {
                glm::vec3 current = std::any_cast<glm::vec3>(value);
                changed = ImGui::ColorEdit3(field.metadata.displayName.c_str(), &current.x);
                if (changed && m_undoSystem) {
                    const auto setter = field.setter;
                    const std::any oldValue = value;
                    const std::any newValue = current;
                    m_undoSystem->execute_or_merge_property(
                        std::string(componentName) + "." + field.metadata.displayName,
                        [component, setter, newValue] { setter(component, newValue); },
                        [component, setter, oldValue] { setter(component, oldValue); },
                        !ImGui::IsItemActivated());
                }
                break;
            }
            default:
                ImGui::TextDisabled("%s: unsupported editor field", field.metadata.displayName.c_str());
                break;
            }
        }
        ImGui::TreePop();
    };

    if (m_activeScene->lightComponents.contains(id)) {
        draw_component("LightComponent", &m_activeScene->lightComponents[id]);
        if (ImGui::Button("Remove LightComponent")) {
            const LightComponent previous = m_activeScene->lightComponents.at(id);
            m_undoSystem->execute_command(std::make_unique<RemoveComponentCommand>(
                "Remove LightComponent",
                [scene = m_activeScene, id] { scene->lightComponents.erase(id); },
                [scene = m_activeScene, id, previous] { scene->lightComponents[id] = previous; }));
        }
    }
    if (m_activeScene->cameraComponents.contains(id)) {
        draw_component("CameraComponent", &m_activeScene->cameraComponents[id]);
        if (ImGui::Button("Remove CameraComponent")) {
            const CameraComponent previous = m_activeScene->cameraComponents.at(id);
            m_undoSystem->execute_command(std::make_unique<RemoveComponentCommand>(
                "Remove CameraComponent",
                [scene = m_activeScene, id] { scene->cameraComponents.erase(id); },
                [scene = m_activeScene, id, previous] { scene->cameraComponents[id] = previous; }));
        }
    }
    if (m_activeScene->rigidbodyComponents.contains(id)) {
        draw_component("RigidbodyComponent", &m_activeScene->rigidbodyComponents[id]);
        if (ImGui::Button("Remove RigidbodyComponent")) {
            const RigidbodyComponent previous = m_activeScene->rigidbodyComponents.at(id);
            m_undoSystem->execute_command(std::make_unique<RemoveComponentCommand>(
                "Remove RigidbodyComponent",
                [scene = m_activeScene, id] { scene->rigidbodyComponents.erase(id); },
                [scene = m_activeScene, id, previous] { scene->rigidbodyComponents[id] = previous; }));
        }
    }
    if (m_activeScene->materialComponents.contains(id)) {
        draw_component("MaterialComponent", &m_activeScene->materialComponents[id]);
        if (ImGui::Button("Remove MaterialComponent")) {
            const MaterialComponent previous = m_activeScene->materialComponents.at(id);
            m_undoSystem->execute_command(std::make_unique<RemoveComponentCommand>(
                "Remove MaterialComponent",
                [scene = m_activeScene, id] { scene->materialComponents.erase(id); },
                [scene = m_activeScene, id, previous] { scene->materialComponents[id] = previous; }));
        }
    }

    ImGui::End();
}

void EditorGUI::draw_content_browser() {
    // Content browser rendering assets, prefabs, materials and scenes
}

void EditorGUI::draw_voxel_editor_panel() {
    // Voxel Brush editor: Brush Shape (Sphere, Cube, Cylinder), Mode (Add, Remove, Replace), Radius, Block Type
}

void EditorGUI::draw_console() {
    // Console log output and performance stats
}

} // namespace Engine
