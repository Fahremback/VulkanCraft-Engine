#pragma once

#include "../core/uuid/UUID.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>

namespace Engine {

enum class MissionState { Inactive, Active, Completed, Failed };
struct MissionStep { std::string signal; uint32_t requiredCount{1}; };
struct MissionDefinition { UUID id; std::string name; std::vector<MissionStep> steps; };

class MissionRuntime final {
public:
    explicit MissionRuntime(MissionDefinition definition);
    void activate();
    void fail();
    bool signal(const std::string& signal);
    MissionState state() const noexcept { return state_; }
    size_t current_step() const noexcept { return currentStep_; }
    uint32_t current_count() const noexcept { return currentCount_; }
    bool save(const std::filesystem::path& path) const;
    bool load(const std::filesystem::path& path);
private:
    MissionDefinition definition_;
    MissionState state_{MissionState::Inactive};
    size_t currentStep_{};
    uint32_t currentCount_{};
};

struct DialogueChoice { std::string text; std::string target; std::string condition; };
struct DialogueNode { std::string id; std::string text; std::vector<DialogueChoice> choices; };
struct DialogueGraph { std::vector<DialogueNode> nodes; std::string entry; };

class DialogueRuntime final {
public:
    explicit DialogueRuntime(DialogueGraph graph);
    void set_condition(std::string name, bool value);
    std::string text() const;
    std::vector<DialogueChoice> available_choices() const;
    bool choose(size_t availableChoiceIndex);
    bool finished() const noexcept { return current_.empty(); }
private:
    const DialogueNode* find(const std::string& id) const;
    DialogueGraph graph_;
    std::string current_;
    std::unordered_map<std::string, bool> conditions_;
};

struct TriggerVolume {
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{0.5f};
    uint32_t layerMask{~0u};
    std::function<void(UUID)> onEnter;
    std::function<void(UUID)> onStay;
    std::function<void(UUID)> onExit;
    void update(UUID entity, const glm::vec3& position, uint32_t entityLayer = 1);
    bool contains(const glm::vec3& position) const;
private:
    std::unordered_set<UUID> inside_;
};

struct Interactable {
    UUID entity;
    glm::vec3 position{0.0f};
    float radius{1.0f};
    std::function<void(UUID)> action;
    bool enabled{true};
};

class InteractionSystem final {
public:
    bool register_interactable(Interactable interactable);
    bool unregister_interactable(UUID entity);
    bool interact(UUID instigator, const glm::vec3& position);
    std::vector<UUID> query(const glm::vec3& position) const;
private:
    std::unordered_map<UUID, Interactable> interactables_;
};

class QuestJournal final {
public:
    bool add(MissionDefinition definition);
    MissionRuntime* mission(UUID id);
    void signal_all(const std::string& signal);
    bool save(const std::filesystem::path& path) const;
private:
    std::unordered_map<UUID, MissionRuntime> missions_;
};

} // namespace Engine
