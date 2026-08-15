#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Dialogue System (README section 28). A dialogue is a graph of nodes, each
// carrying a line (Character / Text / Audio / Animation / Camera) and a set
// of choices. Choices can be gated by named conditions and can branch to
// arbitrary nodes. Integration with the Mission System happens exclusively
// through std::function hooks (missions play dialogues via callbacks, and
// dialogue choices feed events back to missions) — there is no circular
// dependency.

namespace Engine::Gameplay {

struct DialogueLine {
    std::string character;
    std::string text;
    std::string audio;
    std::string animation;
    std::string camera;
};

struct DialogueChoice {
    std::string text;
    std::string nextNode;   // empty = end dialogue
    std::string condition;  // empty = always available
};

struct DialogueNode {
    std::string id;
    DialogueLine line;
    std::vector<DialogueChoice> choices;
};

struct DialogueGraph {
    std::string id;
    std::string entry;
    std::vector<DialogueNode> nodes;
};

// A running dialogue instance over a registered graph.
class Dialogue final {
public:
    explicit Dialogue(const DialogueGraph& graph);

    bool start();
    void stop();
    bool is_running() const noexcept { return running_; }
    bool finished() const noexcept { return !running_; }

    const DialogueGraph& graph() const noexcept { return *graph_; }
    const DialogueNode* current() const noexcept { return current_; }
    bool has_current() const noexcept { return current_ != nullptr; }

    const DialogueLine& current_line() const;
    const std::string& current_character() const;
    const std::string& current_text() const;
    const std::string& current_audio() const;
    const std::string& current_animation() const;
    const std::string& current_camera() const;

    // Choices whose condition is met (empty condition is always available).
    std::vector<const DialogueChoice*> available_choices(
        const std::function<bool(const std::string&)>& conditionLookup) const;

    // Advances to `choice.nextNode`; returns false if the dialogue ended.
    bool choose(const DialogueChoice& choice);

private:
    const DialogueNode* find(const std::string& id) const;

    const DialogueGraph* graph_;
    const DialogueNode* current_{nullptr};
    bool running_{false};
};

class DialogueSystem final {
public:
    bool register_graph(DialogueGraph graph);
    bool unregister(const std::string& id);
    void clear();
    const DialogueGraph* graph(const std::string& id) const;

    bool play(const std::string& dialogueId);
    void stop();
    bool is_playing() const noexcept { return active_ && active_->is_running(); }
    Dialogue* active_dialogue() noexcept { return active_.get(); }
    const Dialogue* active_dialogue() const noexcept { return active_.get(); }
    const std::string* active_dialogue_id() const;

    bool choose(const std::string& choiceText);

    void set_condition(const std::string& name, bool value);
    bool condition(const std::string& name) const;
    void set_condition_lookup(std::function<bool(const std::string&)> callback);

    // Mission integration hooks (missions drive dialogues through callbacks;
    // dialogue events can be fed back into missions by the game code).
    void on_dialogue_started(std::function<void(const std::string&)> callback);    // (dialogueId)
    void on_dialogue_finished(std::function<void(const std::string&)> callback);   // (dialogueId)
    void on_line_spoken(std::function<void(const std::string&, const DialogueLine&)> callback);
    void on_choice_made(std::function<void(const std::string&, const std::string&)> callback);
    //                                                                 (dialogueId, choiceText)

private:
    std::unordered_map<std::string, DialogueGraph> graphs_;
    std::unordered_map<std::string, bool> conditions_;
    std::function<bool(const std::string&)> conditionLookup_;
    std::unique_ptr<Dialogue> active_;
    std::string activeId_;
    std::function<void(const std::string&)> onDialogueStarted_;
    std::function<void(const std::string&)> onDialogueFinished_;
    std::function<void(const std::string&, const DialogueLine&)> onLineSpoken_;
    std::function<void(const std::string&, const std::string&)> onChoiceMade_;
};

} // namespace Engine::Gameplay
