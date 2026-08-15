#include "MissionSystem.hpp"

#include <algorithm>
#include <type_traits>

namespace Engine::Gameplay {
namespace {

double to_number(const MissionValue& value) {
    return std::visit(
        [](const auto& v) -> double {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? 1.0 : 0.0;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return static_cast<double>(v);
            } else if constexpr (std::is_same_v<T, double>) {
                return v;
            } else {
                return 0.0;
            }
        },
        value);
}

bool to_bool(const MissionValue& value) {
    return std::visit(
        [](const auto& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return v != 0;
            } else if constexpr (std::is_same_v<T, double>) {
                return v != 0.0;
            } else {
                return !v.empty();
            }
        },
        value);
}

std::string to_string(const MissionValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                return std::to_string(v);
            } else {
                return v;
            }
        },
        value);
}

bool compare_values(const MissionValue& a, const MissionValue& b, MissionCompare compare) {
    const bool aNumeric = std::holds_alternative<int64_t>(a) || std::holds_alternative<double>(a);
    const bool bNumeric = std::holds_alternative<int64_t>(b) || std::holds_alternative<double>(b);
    if (aNumeric && bNumeric) {
        const double x = to_number(a);
        const double y = to_number(b);
        switch (compare) {
            case MissionCompare::Equal: return x == y;
            case MissionCompare::NotEqual: return x != y;
            case MissionCompare::Greater: return x > y;
            case MissionCompare::Less: return x < y;
            case MissionCompare::GreaterEqual: return x >= y;
            case MissionCompare::LessEqual: return x <= y;
        }
        return false;
    }
    if (std::holds_alternative<bool>(a) && std::holds_alternative<bool>(b)) {
        const bool x = std::get<bool>(a);
        const bool y = std::get<bool>(b);
        switch (compare) {
            case MissionCompare::Equal: return x == y;
            case MissionCompare::NotEqual: return x != y;
            default: return false;
        }
    }
    const std::string x = to_string(a);
    const std::string y = to_string(b);
    switch (compare) {
        case MissionCompare::Equal: return x == y;
        case MissionCompare::NotEqual: return x != y;
        case MissionCompare::Greater: return x > y;
        case MissionCompare::Less: return x < y;
        case MissionCompare::GreaterEqual: return x >= y;
        case MissionCompare::LessEqual: return x <= y;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Node builders
// ---------------------------------------------------------------------------

MissionNode start_node(std::string id, std::string next) {
    MissionNode node;
    node.type = MissionNodeType::Start;
    node.id = std::move(id);
    node.next = std::move(next);
    return node;
}

MissionNode set_objective_node(std::string id, std::string objectiveId,
                               std::string objectiveText, uint32_t objectiveTarget,
                               std::string next) {
    MissionNode node;
    node.type = MissionNodeType::SetObjective;
    node.id = std::move(id);
    node.objectiveId = std::move(objectiveId);
    node.objectiveText = std::move(objectiveText);
    node.objectiveTarget = objectiveTarget;
    node.next = std::move(next);
    return node;
}

MissionNode wait_for_event_node(std::string id, std::string eventName, uint32_t eventCount,
                                std::string next) {
    MissionNode node;
    node.type = MissionNodeType::WaitForEvent;
    node.id = std::move(id);
    node.eventName = std::move(eventName);
    node.eventCount = eventCount;
    node.next = std::move(next);
    return node;
}

MissionNode branch_node(std::string id, std::string variable, MissionCompare compare,
                        MissionValue branchValue, std::string trueBranch,
                        std::string falseBranch) {
    MissionNode node;
    node.type = MissionNodeType::Branch;
    node.id = std::move(id);
    node.variable = std::move(variable);
    node.compare = compare;
    node.branchValue = std::move(branchValue);
    node.trueBranch = std::move(trueBranch);
    node.falseBranch = std::move(falseBranch);
    return node;
}

MissionNode set_variable_node(std::string id, std::string variable, MissionValue value,
                              std::string next) {
    MissionNode node;
    node.type = MissionNodeType::SetVariable;
    node.id = std::move(id);
    node.variable = std::move(variable);
    node.value = std::move(value);
    node.next = std::move(next);
    return node;
}

MissionNode spawn_entity_node(std::string id, std::string entityType, uint32_t spawnCount,
                              std::string next) {
    MissionNode node;
    node.type = MissionNodeType::SpawnEntity;
    node.id = std::move(id);
    node.entityType = std::move(entityType);
    node.spawnCount = spawnCount;
    node.next = std::move(next);
    return node;
}

MissionNode play_dialogue_node(std::string id, std::string dialogueId, std::string next) {
    MissionNode node;
    node.type = MissionNodeType::PlayDialogue;
    node.id = std::move(id);
    node.dialogueId = std::move(dialogueId);
    node.next = std::move(next);
    return node;
}

MissionNode give_item_node(std::string id, std::string itemId, uint32_t itemQuantity,
                           std::string next) {
    MissionNode node;
    node.type = MissionNodeType::GiveItem;
    node.id = std::move(id);
    node.itemId = std::move(itemId);
    node.itemQuantity = itemQuantity;
    node.next = std::move(next);
    return node;
}

MissionNode complete_mission_node(std::string id) {
    MissionNode node;
    node.type = MissionNodeType::CompleteMission;
    node.id = std::move(id);
    return node;
}

MissionNode fail_mission_node(std::string id) {
    MissionNode node;
    node.type = MissionNodeType::FailMission;
    node.id = std::move(id);
    return node;
}

// ---------------------------------------------------------------------------
// Mission
// ---------------------------------------------------------------------------

Mission::Mission(std::string id, std::string name, std::vector<MissionNode> nodes,
                 std::string entry)
    : id_(std::move(id)),
      name_(std::move(name)),
      entry_(std::move(entry)),
      nodes_(std::move(nodes)) {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        index_[nodes_[i].id] = i;
    }
    if (entry_.empty() && index_.count("start") > 0) {
        entry_ = "start";
    }
}

void Mission::start() {
    if (state_ == MissionState::Active || state_ == MissionState::Completed) {
        return;
    }
    if (index_.empty()) {
        return;
    }
    state_ = MissionState::Active;
    current_ = nullptr;
    next_ = entry_;
    pendingEvents_ = 0;
    advance();
}

void Mission::fail() {
    if (state_ == MissionState::Completed || state_ == MissionState::Failed) {
        return;
    }
    state_ = MissionState::Failed;
    if (onFailed_) {
        onFailed_();
    }
}

void Mission::update(float) {
    // Missions are event-driven; this hook exists for future timed nodes.
}

void Mission::handle_event(const std::string& name, const std::any& payload) {
    (void)payload;
    if (state_ != MissionState::Active || !current_) {
        return;
    }
    if (current_->type != MissionNodeType::WaitForEvent || current_->eventName != name) {
        return;
    }
    if (pendingEvents_ > 0) {
        --pendingEvents_;
    }
    if (pendingEvents_ == 0) {
        next_ = current_->next;
        current_ = nullptr;
        advance();
    }
}

void Mission::set_variable(const std::string& name, const MissionValue& value) {
    variables_[name] = value;
}

const MissionValue* Mission::variable(const std::string& name) const {
    const auto it = variables_.find(name);
    return it == variables_.end() ? nullptr : &it->second;
}

bool Mission::variable_bool(const std::string& name) const {
    const MissionValue* value = variable(name);
    return value ? to_bool(*value) : false;
}

double Mission::variable_number(const std::string& name) const {
    const MissionValue* value = variable(name);
    return value ? to_number(*value) : 0.0;
}

std::string Mission::variable_string(const std::string& name) const {
    const MissionValue* value = variable(name);
    return value ? to_string(*value) : std::string();
}

void Mission::on_objective(std::function<void(const std::string&, const std::string&, uint32_t)> callback) {
    onObjective_ = std::move(callback);
}

void Mission::on_spawn(std::function<void(const std::string&, uint32_t)> callback) {
    onSpawn_ = std::move(callback);
}

void Mission::on_play_dialogue(std::function<void(const std::string&)> callback) {
    onDialogue_ = std::move(callback);
}

void Mission::on_give_item(std::function<void(const std::string&, uint32_t)> callback) {
    onGiveItem_ = std::move(callback);
}

void Mission::on_completed(std::function<void()> callback) {
    onCompleted_ = std::move(callback);
}

void Mission::on_failed(std::function<void()> callback) {
    onFailed_ = std::move(callback);
}

void Mission::advance() {
    while (state_ == MissionState::Active) {
        if (next_.empty()) {
            // Flow exhausted without an explicit completion node.
            state_ = MissionState::Completed;
            if (onCompleted_) {
                onCompleted_();
            }
            return;
        }
        const MissionNode* node = find(next_);
        if (!node) {
            state_ = MissionState::Failed;
            if (onFailed_) {
                onFailed_();
            }
            return;
        }
        current_ = node;
        if (execute(*node)) {
            return;   // blocking node reached
        }
    }
}

bool Mission::execute(const MissionNode& node) {
    switch (node.type) {
        case MissionNodeType::Start:
            next_ = node.next;
            return false;
        case MissionNodeType::SetObjective:
            if (onObjective_) {
                onObjective_(node.objectiveId, node.objectiveText, node.objectiveTarget);
            }
            next_ = node.next;
            return false;
        case MissionNodeType::WaitForEvent:
            pendingEvents_ = node.eventCount > 0 ? node.eventCount : 1;
            return true;
        case MissionNodeType::Branch:
            next_ = evaluate_branch(node) ? node.trueBranch : node.falseBranch;
            return false;
        case MissionNodeType::SetVariable:
            variables_[node.variable] = node.value;
            next_ = node.next;
            return false;
        case MissionNodeType::SpawnEntity:
            if (onSpawn_) {
                onSpawn_(node.entityType, node.spawnCount);
            }
            next_ = node.next;
            return false;
        case MissionNodeType::PlayDialogue:
            if (onDialogue_) {
                onDialogue_(node.dialogueId);
            }
            next_ = node.next;
            return false;
        case MissionNodeType::GiveItem:
            if (onGiveItem_) {
                onGiveItem_(node.itemId, node.itemQuantity);
            }
            next_ = node.next;
            return false;
        case MissionNodeType::CompleteMission:
            state_ = MissionState::Completed;
            if (onCompleted_) {
                onCompleted_();
            }
            return true;
        case MissionNodeType::FailMission:
            state_ = MissionState::Failed;
            if (onFailed_) {
                onFailed_();
            }
            return true;
    }
    next_ = node.next;
    return false;
}

bool Mission::evaluate_branch(const MissionNode& node) const {
    MissionValue lhs{false};
    if (const MissionValue* value = variable(node.variable)) {
        lhs = *value;
    } else if (std::holds_alternative<int64_t>(node.branchValue) ||
               std::holds_alternative<double>(node.branchValue)) {
        lhs = MissionValue{0.0};
    }
    return compare_values(lhs, node.branchValue, node.compare);
}

const MissionNode* Mission::find(const std::string& id) const {
    const auto it = index_.find(id);
    if (it == index_.end() || it->second >= nodes_.size()) {
        return nullptr;
    }
    return &nodes_[it->second];
}

// ---------------------------------------------------------------------------
// MissionSystem
// ---------------------------------------------------------------------------

bool MissionSystem::register_mission(Mission mission) {
    if (mission.id().empty()) {
        return false;
    }
    missions_.insert_or_assign(mission.id(), std::move(mission));
    return true;
}

bool MissionSystem::unregister(const std::string& id) {
    return missions_.erase(id) > 0;
}

void MissionSystem::clear() {
    missions_.clear();
}

Mission* MissionSystem::mission(const std::string& id) {
    const auto it = missions_.find(id);
    return it == missions_.end() ? nullptr : &it->second;
}

const Mission* MissionSystem::mission(const std::string& id) const {
    const auto it = missions_.find(id);
    return it == missions_.end() ? nullptr : &it->second;
}

bool MissionSystem::start(const std::string& id) {
    const auto it = missions_.find(id);
    if (it == missions_.end()) {
        return false;
    }
    const MissionState before = it->second.state();
    it->second.start();
    // True if the mission was actually launched (it may already have finished
    // synchronously when its graph has no blocking nodes).
    return before == MissionState::Inactive;
}

bool MissionSystem::fail(const std::string& id) {
    const auto it = missions_.find(id);
    if (it == missions_.end()) {
        return false;
    }
    it->second.fail();
    return true;
}

void MissionSystem::update(float deltaSeconds) {
    for (auto& [id, mission] : missions_) {
        (void)id;
        mission.update(deltaSeconds);
    }
}

void MissionSystem::dispatch_event(const std::string& name, const std::any& payload) {
    for (auto& [id, mission] : missions_) {
        (void)id;
        if (mission.state() == MissionState::Active) {
            mission.handle_event(name, payload);
        }
    }
}

std::vector<std::string> MissionSystem::active_missions() const {
    std::vector<std::string> result;
    for (const auto& [id, mission] : missions_) {
        (void)id;
        if (mission.state() == MissionState::Active) {
            result.push_back(mission.id());
        }
    }
    return result;
}

} // namespace Engine::Gameplay
