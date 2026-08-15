#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// Graph-based Mission System (README section 27). Missions are composed of
// nodes (Start, SetObjective, WaitForEvent, Branch, SetVariable, SpawnEntity,
// PlayDialogue, GiveItem, CompleteMission, FailMission) and execute step by
// step while observing game events. Missions never touch chunks, renderers or
// internal entity memory; they talk to gameplay systems exclusively through
// std::function hooks, so there is no dependency on DialogueSystem (and no
// circular dependency between the two).

namespace Engine::Gameplay {

enum class MissionState : uint8_t { Inactive, Active, Completed, Failed };
enum class MissionNodeType : uint8_t {
    Start,
    SetObjective,
    WaitForEvent,
    Branch,
    SetVariable,
    SpawnEntity,
    PlayDialogue,
    GiveItem,
    CompleteMission,
    FailMission
};
enum class MissionCompare : uint8_t { Equal, NotEqual, Greater, Less, GreaterEqual, LessEqual };

using MissionValue = std::variant<bool, int64_t, double, std::string>;

struct MissionNode {
    MissionNodeType type{MissionNodeType::Start};
    std::string id;
    std::string next;   // linear successor id (empty = flow ends)

    // SetObjective
    std::string objectiveId;
    std::string objectiveText;
    uint32_t objectiveTarget{1};

    // WaitForEvent
    std::string eventName;
    uint32_t eventCount{1};

    // Branch
    std::string variable;
    MissionCompare compare{MissionCompare::Equal};
    MissionValue branchValue{false};
    std::string trueBranch;
    std::string falseBranch;

    // SetVariable
    MissionValue value{false};

    // SpawnEntity
    std::string entityType;
    uint32_t spawnCount{1};

    // PlayDialogue
    std::string dialogueId;

    // GiveItem
    std::string itemId;
    uint32_t itemQuantity{1};
};

// Node builder helpers (kept as free functions for readable graph definitions).
MissionNode start_node(std::string id, std::string next = "");
MissionNode set_objective_node(std::string id, std::string objectiveId,
                               std::string objectiveText, uint32_t objectiveTarget,
                               std::string next);
MissionNode wait_for_event_node(std::string id, std::string eventName, uint32_t eventCount,
                                std::string next);
MissionNode branch_node(std::string id, std::string variable, MissionCompare compare,
                        MissionValue branchValue, std::string trueBranch,
                        std::string falseBranch);
MissionNode set_variable_node(std::string id, std::string variable, MissionValue value,
                              std::string next);
MissionNode spawn_entity_node(std::string id, std::string entityType, uint32_t spawnCount,
                              std::string next);
MissionNode play_dialogue_node(std::string id, std::string dialogueId, std::string next);
MissionNode give_item_node(std::string id, std::string itemId, uint32_t itemQuantity,
                           std::string next);
MissionNode complete_mission_node(std::string id);
MissionNode fail_mission_node(std::string id);

class Mission final {
public:
    Mission() = default;
    Mission(std::string id, std::string name, std::vector<MissionNode> nodes,
            std::string entry = "start");

    void start();
    void fail();
    void update(float deltaSeconds);   // hook for future timed nodes

    void handle_event(const std::string& name, const std::any& payload = {});

    void set_variable(const std::string& name, const MissionValue& value);
    const MissionValue* variable(const std::string& name) const;
    bool variable_bool(const std::string& name) const;
    double variable_number(const std::string& name) const;
    std::string variable_string(const std::string& name) const;

    MissionState state() const noexcept { return state_; }
    const std::string& id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    bool is_active() const noexcept { return state_ == MissionState::Active; }
    const std::string* current_node() const noexcept { return current_ ? &current_->id : nullptr; }

    // External system hooks (set by the game or by MissionSystem users).
    void on_objective(std::function<void(const std::string&, const std::string&, uint32_t)> callback);
    void on_spawn(std::function<void(const std::string&, uint32_t)> callback);
    void on_play_dialogue(std::function<void(const std::string&)> callback);
    void on_give_item(std::function<void(const std::string&, uint32_t)> callback);
    void on_completed(std::function<void()> callback);
    void on_failed(std::function<void()> callback);

private:
    void advance();
    bool execute(const MissionNode& node);   // true = blocking (WaitForEvent)
    bool evaluate_branch(const MissionNode& node) const;
    const MissionNode* find(const std::string& id) const;

    std::string id_;
    std::string name_;
    std::string entry_;
    std::vector<MissionNode> nodes_;
    std::unordered_map<std::string, size_t> index_;
    const MissionNode* current_{nullptr};
    std::string next_;
    uint32_t pendingEvents_{0};
    MissionState state_{MissionState::Inactive};
    std::unordered_map<std::string, MissionValue> variables_;
    std::function<void(const std::string&, const std::string&, uint32_t)> onObjective_;
    std::function<void(const std::string&, uint32_t)> onSpawn_;
    std::function<void(const std::string&)> onDialogue_;
    std::function<void(const std::string&, uint32_t)> onGiveItem_;
    std::function<void()> onCompleted_;
    std::function<void()> onFailed_;
};

class MissionSystem final {
public:
    bool register_mission(Mission mission);
    bool unregister(const std::string& id);
    void clear();
    Mission* mission(const std::string& id);
    const Mission* mission(const std::string& id) const;

    bool start(const std::string& id);
    bool fail(const std::string& id);
    void update(float deltaSeconds);
    void dispatch_event(const std::string& name, const std::any& payload = {});

    std::vector<std::string> active_missions() const;
    size_t mission_count() const noexcept { return missions_.size(); }

private:
    std::unordered_map<std::string, Mission> missions_;
};

} // namespace Engine::Gameplay
