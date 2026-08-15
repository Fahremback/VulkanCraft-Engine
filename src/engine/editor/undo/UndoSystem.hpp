#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <glm/glm.hpp>
#include "../../scene/Scene.hpp"

namespace Engine {

class EditorCommand {
public:
    virtual ~EditorCommand() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual std::string get_name() const = 0;
};

class PropertyChangeCommand : public EditorCommand {
public:
    PropertyChangeCommand(std::string name, std::function<void()> execute,
                          std::function<void()> undo)
        : m_name(std::move(name)), m_execute(std::move(execute)), m_undo(std::move(undo)) {}

    void execute() override { m_execute(); }
    void undo() override { m_undo(); }
    std::string get_name() const override { return m_name; }
    void update_execute(std::function<void()> execute) { m_execute = std::move(execute); }

private:
    std::string m_name;
    std::function<void()> m_execute;
    std::function<void()> m_undo;
};

class ComponentCommand : public EditorCommand {
public:
    ComponentCommand(std::string name, std::function<void()> execute,
                     std::function<void()> undo)
        : m_name(std::move(name)), m_execute(std::move(execute)), m_undo(std::move(undo)) {}

    void execute() override { m_execute(); }
    void undo() override { m_undo(); }
    std::string get_name() const override { return m_name; }

private:
    std::string m_name;
    std::function<void()> m_execute;
    std::function<void()> m_undo;
};

class AddComponentCommand final : public ComponentCommand {
public:
    AddComponentCommand(std::string name, std::function<void()> execute,
                        std::function<void()> undo)
        : ComponentCommand(std::move(name), std::move(execute), std::move(undo)) {}
};

class RemoveComponentCommand final : public ComponentCommand {
public:
    RemoveComponentCommand(std::string name, std::function<void()> execute,
                           std::function<void()> undo)
        : ComponentCommand(std::move(name), std::move(execute), std::move(undo)) {}
};

class CreateEntityCommand : public EditorCommand {
public:
    CreateEntityCommand(Scene* scene, const std::string& name)
        : m_scene(scene), m_name(name) {}

    void execute() override {
        if (!m_createdID.is_valid()) {
            Entity ent = m_scene->create_entity(m_name);
            m_createdID = ent.get_id();
        } else {
            m_scene->create_entity_with_id(m_createdID, m_name);
        }
    }

    void undo() override {
        if (m_createdID.is_valid()) {
            m_scene->destroy_entity(m_createdID);
        }
    }

    std::string get_name() const override {
        return "Create Entity '" + m_name + "'";
    }

private:
    Scene* m_scene{ nullptr };
    std::string m_name;
    UUID m_createdID{ 0, 0 };
};

class MoveEntityCommand : public EditorCommand {
public:
    MoveEntityCommand(Scene* scene, UUID entityID, const glm::vec3& oldPos, const glm::vec3& newPos)
        : m_scene(scene), m_entityID(entityID), m_oldPos(oldPos), m_newPos(newPos) {}

    void execute() override {
        if (m_scene->transformComponents.contains(m_entityID)) {
            m_scene->transformComponents[m_entityID].position = m_newPos;
        }
    }

    void undo() override {
        if (m_scene->transformComponents.contains(m_entityID)) {
            m_scene->transformComponents[m_entityID].position = m_oldPos;
        }
    }

    std::string get_name() const override {
        return "Move Entity";
    }

private:
    Scene* m_scene;
    UUID m_entityID;
    glm::vec3 m_oldPos;
    glm::vec3 m_newPos;
};

class DeleteEntityCommand : public EditorCommand {
public:
    DeleteEntityCommand(Scene* scene, UUID entityID)
        : m_scene(scene), m_entityID(entityID) {
        if (!m_scene) return;
        Entity ent = m_scene->find_entity_by_id(entityID);
        if (ent.is_valid()) {
            m_name = ent.get_name();
        }
        if (m_scene->transformComponents.contains(entityID)) {
            m_hasTransform = true;
            m_transform = m_scene->transformComponents.at(entityID);
        }
        if (m_scene->meshRendererComponents.contains(entityID)) {
            m_hasMesh = true;
            m_mesh = m_scene->meshRendererComponents.at(entityID);
        }
        if (m_scene->lightComponents.contains(entityID)) {
            m_hasLight = true;
            m_light = m_scene->lightComponents.at(entityID);
        }
        if (m_scene->cameraComponents.contains(entityID)) {
            m_hasCamera = true;
            m_camera = m_scene->cameraComponents.at(entityID);
        }
        if (m_scene->rigidbodyComponents.contains(entityID)) {
            m_hasRigidbody = true;
            m_rigidbody = m_scene->rigidbodyComponents.at(entityID);
        }
        if (m_scene->materialComponents.contains(entityID)) {
            m_hasMaterial = true;
            m_material = m_scene->materialComponents.at(entityID);
        }
        if (m_scene->hierarchyComponents.contains(entityID)) {
            m_hasHierarchy = true;
            m_parentID = m_scene->hierarchyComponents.at(entityID).parentID;
            m_childrenIDs = m_scene->hierarchyComponents.at(entityID).childrenIDs;
        }
        if (m_scene->voxelVolumeComponents.contains(entityID)) {
            m_hasVoxel = true;
            m_voxel = m_scene->voxelVolumeComponents.at(entityID);
        }
    }

    void execute() override {
        if (m_scene) {
            m_scene->destroy_entity(m_entityID);
        }
    }

    void undo() override {
        if (m_scene) {
            m_scene->create_entity_with_id(m_entityID, m_name);
            if (m_hasTransform) m_scene->transformComponents[m_entityID] = m_transform;
            if (m_hasMesh) m_scene->meshRendererComponents[m_entityID] = m_mesh;
            if (m_hasLight) m_scene->lightComponents[m_entityID] = m_light;
            if (m_hasCamera) m_scene->cameraComponents[m_entityID] = m_camera;
            if (m_hasRigidbody) m_scene->rigidbodyComponents[m_entityID] = m_rigidbody;
            if (m_hasMaterial) m_scene->materialComponents[m_entityID] = m_material;
            if (m_hasHierarchy) {
                m_scene->set_parent(m_entityID, m_parentID);
                for (UUID childID : m_childrenIDs) {
                    m_scene->set_parent(childID, m_entityID);
                }
            }
            if (m_hasVoxel) m_scene->voxelVolumeComponents[m_entityID] = m_voxel;
        }
    }

    std::string get_name() const override {
        return "Delete Entity '" + m_name + "'";
    }

private:
    Scene* m_scene{ nullptr };
    UUID m_entityID{ 0, 0 };
    std::string m_name;

    bool m_hasTransform{ false };
    TransformComponent m_transform;
    bool m_hasMesh{ false };
    MeshRendererComponent m_mesh;
    bool m_hasLight{ false };
    LightComponent m_light;
    bool m_hasCamera{ false };
    CameraComponent m_camera;
    bool m_hasRigidbody{ false };
    RigidbodyComponent m_rigidbody;
    bool m_hasMaterial{ false };
    MaterialComponent m_material;
    bool m_hasHierarchy{ false };
    UUID m_parentID{ 0, 0 };
    std::vector<UUID> m_childrenIDs;
    bool m_hasVoxel{ false };
    VoxelVolumeComponent m_voxel;
};

class ReparentEntityCommand : public EditorCommand {
public:
    ReparentEntityCommand(Scene* scene, UUID entityID, UUID oldParentID, UUID newParentID)
        : m_scene(scene), m_entityID(entityID), m_oldParentID(oldParentID), m_newParentID(newParentID) {}

    void execute() override {
        if (m_scene) m_scene->set_parent(m_entityID, m_newParentID);
    }

    void undo() override {
        if (m_scene) m_scene->set_parent(m_entityID, m_oldParentID);
    }

    std::string get_name() const override {
        return "Reparent Entity";
    }

private:
    Scene* m_scene{ nullptr };
    UUID m_entityID{ 0, 0 };
    UUID m_oldParentID{ 0, 0 };
    UUID m_newParentID{ 0, 0 };
};

class RenameEntityCommand : public EditorCommand {
public:
    RenameEntityCommand(Scene* scene, UUID entityID, std::string oldName, std::string newName)
        : m_scene(scene), m_entityID(entityID), m_oldName(std::move(oldName)), m_newName(std::move(newName)) {}

    void execute() override {
        if (!m_scene) return;
        m_scene->rename_entity(m_entityID, m_newName);
    }

    void undo() override {
        if (!m_scene) return;
        m_scene->rename_entity(m_entityID, m_oldName);
    }

    std::string get_name() const override {
        return "Rename Entity to '" + m_newName + "'";
    }

private:
    Scene* m_scene{ nullptr };
    UUID m_entityID{ 0, 0 };
    std::string m_oldName;
    std::string m_newName;
};

class UndoSystem {
public:
    UndoSystem() = default;
    UndoSystem(const UndoSystem&) = delete;
    UndoSystem& operator=(const UndoSystem&) = delete;

    void execute_command(std::unique_ptr<EditorCommand> command) {
        command->execute();
        m_undoStack.push_back(std::move(command));
        m_redoStack.clear();
    }

    void execute_or_merge_property(std::string name, std::function<void()> execute,
                                   std::function<void()> undo, bool allowMerge = true) {
        if (allowMerge && !m_undoStack.empty() && m_redoStack.empty()) {
            auto* previous = dynamic_cast<PropertyChangeCommand*>(m_undoStack.back().get());
            if (previous && previous->get_name() == name) {
                previous->update_execute(std::move(execute));
                previous->execute();
                return;
            }
        }
        execute_command(std::make_unique<PropertyChangeCommand>(
            std::move(name), std::move(execute), std::move(undo)));
    }

    bool can_undo() const { return !m_undoStack.empty(); }
    bool can_redo() const { return !m_redoStack.empty(); }

    void undo() {
        if (!can_undo()) return;
        auto command = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        command->undo();
        m_redoStack.push_back(std::move(command));
    }

    void redo() {
        if (!can_redo()) return;
        auto command = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        command->execute();
        m_undoStack.push_back(std::move(command));
    }

    void clear() {
        m_undoStack.clear();
        m_redoStack.clear();
    }

private:
    std::vector<std::unique_ptr<EditorCommand>> m_undoStack;
    std::vector<std::unique_ptr<EditorCommand>> m_redoStack;
};

} // namespace Engine
