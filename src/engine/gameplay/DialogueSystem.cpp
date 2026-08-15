#include "DialogueSystem.hpp"

#include <utility>

namespace Engine::Gameplay {
namespace {

const DialogueLine kEmptyLine;

} // namespace

// ---------------------------------------------------------------------------
// Dialogue
// ---------------------------------------------------------------------------

Dialogue::Dialogue(const DialogueGraph& graph) : graph_(&graph) {}

bool Dialogue::start() {
    current_ = find(graph_->entry);
    if (!current_) {
        return false;
    }
    // A dialogue whose entry node has no choices is immediately over but the
    // line stays readable.
    running_ = !current_->choices.empty();
    return true;
}

void Dialogue::stop() {
    current_ = nullptr;
    running_ = false;
}

const DialogueLine& Dialogue::current_line() const {
    return current_ ? current_->line : kEmptyLine;
}

const std::string& Dialogue::current_character() const {
    return current_line().character;
}

const std::string& Dialogue::current_text() const {
    return current_line().text;
}

const std::string& Dialogue::current_audio() const {
    return current_line().audio;
}

const std::string& Dialogue::current_animation() const {
    return current_line().animation;
}

const std::string& Dialogue::current_camera() const {
    return current_line().camera;
}

std::vector<const DialogueChoice*> Dialogue::available_choices(
    const std::function<bool(const std::string&)>& conditionLookup) const {
    std::vector<const DialogueChoice*> result;
    if (!current_) {
        return result;
    }
    for (const DialogueChoice& choice : current_->choices) {
        if (choice.condition.empty() || (conditionLookup && conditionLookup(choice.condition))) {
            result.push_back(&choice);
        }
    }
    return result;
}

bool Dialogue::choose(const DialogueChoice& choice) {
    if (!running_ || !current_) {
        return false;
    }
    const DialogueNode* next = choice.nextNode.empty() ? nullptr : find(choice.nextNode);
    if (!next) {
        current_ = nullptr;
        running_ = false;
        return false;
    }
    current_ = next;
    // A node without choices is the final line of the dialogue; it stays
    // readable but the dialogue is over.
    running_ = !current_->choices.empty();
    return running_;
}

const DialogueNode* Dialogue::find(const std::string& id) const {
    for (const DialogueNode& node : graph_->nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// DialogueSystem
// ---------------------------------------------------------------------------

bool DialogueSystem::register_graph(DialogueGraph graph) {
    if (graph.id.empty()) {
        return false;
    }
    graphs_.insert_or_assign(graph.id, std::move(graph));
    return true;
}

bool DialogueSystem::unregister(const std::string& id) {
    if (active_ && activeId_ == id) {
        active_.reset();
        activeId_.clear();
    }
    return graphs_.erase(id) > 0;
}

void DialogueSystem::clear() {
    graphs_.clear();
    conditions_.clear();
    active_.reset();
    activeId_.clear();
}

const DialogueGraph* DialogueSystem::graph(const std::string& id) const {
    const auto it = graphs_.find(id);
    return it == graphs_.end() ? nullptr : &it->second;
}

bool DialogueSystem::play(const std::string& dialogueId) {
    const auto it = graphs_.find(dialogueId);
    if (it == graphs_.end()) {
        return false;
    }
    if (active_ && active_->is_running() && onDialogueFinished_) {
        onDialogueFinished_(activeId_);
    }
    active_ = std::make_unique<Dialogue>(it->second);
    activeId_ = dialogueId;
    if (!active_->start()) {
        active_.reset();
        activeId_.clear();
        return false;
    }
    if (onDialogueStarted_) {
        onDialogueStarted_(dialogueId);
    }
    if (onLineSpoken_) {
        onLineSpoken_(dialogueId, active_->current_line());
    }
    return true;
}

void DialogueSystem::stop() {
    if (active_ && active_->is_running() && onDialogueFinished_) {
        onDialogueFinished_(activeId_);
    }
    active_.reset();
    activeId_.clear();
}

const std::string* DialogueSystem::active_dialogue_id() const {
    return active_ ? &activeId_ : nullptr;
}

bool DialogueSystem::choose(const std::string& choiceText) {
    if (!active_ || !active_->is_running()) {
        return false;
    }
    const std::string dialogueId = activeId_;
    const auto lookup = [this](const std::string& name) -> bool { return condition(name); };
    const std::vector<const DialogueChoice*> available = active_->available_choices(lookup);
    for (const DialogueChoice* choice : available) {
        if (choice->text != choiceText) {
            continue;
        }
        const bool stillRunning = active_->choose(*choice);
        if (!stillRunning) {
            if (onDialogueFinished_) {
                onDialogueFinished_(dialogueId);
            }
        } else if (onLineSpoken_) {
            onLineSpoken_(dialogueId, active_->current_line());
        }
        if (onChoiceMade_) {
            onChoiceMade_(dialogueId, choiceText);
        }
        return true;
    }
    return false;
}

void DialogueSystem::set_condition(const std::string& name, bool value) {
    conditions_[name] = value;
}

bool DialogueSystem::condition(const std::string& name) const {
    const auto it = conditions_.find(name);
    if (it != conditions_.end()) {
        return it->second;
    }
    if (conditionLookup_) {
        return conditionLookup_(name);
    }
    return false;
}

void DialogueSystem::set_condition_lookup(std::function<bool(const std::string&)> callback) {
    conditionLookup_ = std::move(callback);
}

void DialogueSystem::on_dialogue_started(std::function<void(const std::string&)> callback) {
    onDialogueStarted_ = std::move(callback);
}

void DialogueSystem::on_dialogue_finished(std::function<void(const std::string&)> callback) {
    onDialogueFinished_ = std::move(callback);
}

void DialogueSystem::on_line_spoken(
    std::function<void(const std::string&, const DialogueLine&)> callback) {
    onLineSpoken_ = std::move(callback);
}

void DialogueSystem::on_choice_made(
    std::function<void(const std::string&, const std::string&)> callback) {
    onChoiceMade_ = std::move(callback);
}

} // namespace Engine::Gameplay
