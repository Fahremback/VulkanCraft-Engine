#pragma once

// DialogueEditorModel — UI-independent document model for editing a dialogue
// graph: line nodes with choices (text/target/condition). Amplified with
// undo/redo (snapshot-based), node/choice management, entry switching and
// reachability validation (nodes unreachable from the entry are reported).

#include "EditorToolModel.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine::Editor {

struct DialogueChoiceModel {
    std::string text;
    uint64_t target{};
    std::string condition;
};

struct DialogueNodeModel {
    uint64_t id{};
    std::string speaker, text;
    glm::vec2 position{0};
    std::vector<DialogueChoiceModel> choices;
};

class DialogueEditorModel final : public EditorDocumentModel {
public:
    uint64_t entry{};
    std::vector<DialogueNodeModel> nodes;

    bool add_node(DialogueNodeModel n) {
        if (n.id == 0 || find(n.id)) return false;
        Snapshot before = snapshot();
        nodes.push_back(std::move(n));
        push_command("Add Dialogue Node", before, snapshot());
        return true;
    }

    bool remove_node(uint64_t id) {
        if (!find(id)) return false;
        Snapshot before = snapshot();
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                                   [&](const auto& n) { return n.id == id; }),
                    nodes.end());
        if (entry == id) entry = 0;
        // Drop choices that pointed at the removed node.
        for (auto& n : nodes) {
            n.choices.erase(std::remove_if(n.choices.begin(), n.choices.end(),
                                           [&](const auto& c) { return c.target == id; }),
                            n.choices.end());
        }
        push_command("Remove Dialogue Node", before, snapshot());
        return true;
    }

    bool add_choice(uint64_t nodeId, DialogueChoiceModel choice) {
        DialogueNodeModel* node = find(nodeId);
        if (!node) return false;
        Snapshot before = snapshot();
        node->choices.push_back(std::move(choice));
        push_command("Add Dialogue Choice", before, snapshot());
        return true;
    }

    bool remove_choice(uint64_t nodeId, std::size_t index) {
        DialogueNodeModel* node = find(nodeId);
        if (!node || index >= node->choices.size()) return false;
        Snapshot before = snapshot();
        node->choices.erase(node->choices.begin() + static_cast<std::ptrdiff_t>(index));
        push_command("Remove Dialogue Choice", before, snapshot());
        return true;
    }

    bool set_entry(uint64_t id) {
        if (id != 0 && !find(id)) return false;
        Snapshot before = snapshot();
        entry = id;
        push_command("Set Dialogue Entry", before, snapshot());
        return true;
    }

    bool move_node(uint64_t id, glm::vec2 position) {
        DialogueNodeModel* node = find(id);
        if (!node) return false;
        Snapshot before = snapshot();
        node->position = position;
        push_command("Move Dialogue Node", before, snapshot());
        return true;
    }

    bool update_node(DialogueNodeModel replacement) {
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const auto& n) { return n.id == replacement.id; });
        if (it == nodes.end()) return false;
        Snapshot before = snapshot();
        *it = std::move(replacement);
        push_command("Update Dialogue Node", before, snapshot());
        return true;
    }

    DialogueNodeModel* find(uint64_t id) {
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const auto& n) { return n.id == id; });
        return it == nodes.end() ? nullptr : &*it;
    }
    const DialogueNodeModel* find(uint64_t id) const {
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const auto& n) { return n.id == id; });
        return it == nodes.end() ? nullptr : &*it;
    }

    // --- undo / redo ---
    void undo() {
        if (undoStack_.empty()) return;
        Command command = std::move(undoStack_.back());
        undoStack_.pop_back();
        command.undo();
        redoStack_.push_back(std::move(command));
    }
    void redo() {
        if (redoStack_.empty()) return;
        Command command = std::move(redoStack_.back());
        redoStack_.pop_back();
        command.redo();
        undoStack_.push_back(std::move(command));
    }
    [[nodiscard]] bool can_undo() const noexcept { return !undoStack_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redoStack_.empty(); }
    [[nodiscard]] std::size_t undo_depth() const noexcept { return undoStack_.size(); }
    [[nodiscard]] std::size_t redo_depth() const noexcept { return redoStack_.size(); }
    void clear_undo() noexcept {
        undoStack_.clear();
        redoStack_.clear();
    }

    std::vector<ValidationIssue> validate() const override {
        std::vector<ValidationIssue> issues;
        const auto errors = [&](std::string field, std::string message) {
            issues.push_back({ValidationSeverity::Error, std::move(field), std::move(message)});
        };
        const auto warnings = [&](std::string field, std::string message) {
            issues.push_back({ValidationSeverity::Warning, std::move(field), std::move(message)});
        };

        if (!find(entry)) errors("entry", "Dialogue entry does not exist");
        for (const auto& n : nodes) {
            if (n.text.empty()) {
                warnings(std::to_string(n.id), "Dialogue node has no text");
            }
            for (const auto& c : n.choices) {
                if (c.target && !find(c.target)) {
                    errors(c.text, "Choice target does not exist");
                }
                if (c.target == n.id) {
                    warnings(c.text, "Choice loops back to the same node");
                }
            }
        }
        // Nodes that cannot be reached from the entry will never play.
        if (find(entry)) {
            std::unordered_set<uint64_t> reached;
            std::vector<uint64_t> queue{entry};
            reached.insert(entry);
            while (!queue.empty()) {
                const uint64_t current = queue.back();
                queue.pop_back();
                const DialogueNodeModel* node = find(current);
                if (!node) continue;
                for (const auto& c : node->choices) {
                    if (c.target && reached.insert(c.target).second) queue.push_back(c.target);
                }
            }
            for (const auto& n : nodes) {
                if (n.id != entry && reached.count(n.id) == 0) {
                    warnings(std::to_string(n.id), "Dialogue node is unreachable from the entry");
                }
            }
        }
        return issues;
    }

private:
    struct Snapshot {
        uint64_t entry;
        std::vector<DialogueNodeModel> nodes;
    };
    struct Command {
        std::string name;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    [[nodiscard]] Snapshot snapshot() const { return {entry, nodes}; }
    void restore(const Snapshot& state) {
        entry = state.entry;
        nodes = state.nodes;
    }
    void push_command(std::string name, Snapshot before, Snapshot after) {
        undoStack_.push_back(Command{
            std::move(name),
            [this, before = std::move(before)]() {
                restore(before);
                changed();
            },
            [this, after = std::move(after)]() {
                restore(after);
                changed();
            }});
        redoStack_.clear();
        changed();
    }

    std::vector<Command> undoStack_;
    std::vector<Command> redoStack_;
};

} // namespace Engine::Editor
