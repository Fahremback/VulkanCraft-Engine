#pragma once

// MissionEditorModel — UI-independent document model for editing a mission
// graph (nodes + directed edges). Amplified with undo/redo (snapshot-based),
// node/edge removal, moving and updating, plus stronger validation: exactly
// one entry, dangling edges, self-loops, duplicate edges, cycles and isolated
// nodes. Everything is testable without any UI.

#include "EditorToolModel.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine::Editor {

enum class MissionNodeKind : uint8_t { Entry, Objective, Condition, Reward, Failure, Completion };

struct MissionNodeModel {
    uint64_t id{};
    MissionNodeKind kind{};
    std::string title, payload;
    glm::vec2 position{0};
};

struct MissionEdgeModel {
    uint64_t from{}, to{};
    std::string condition;
};

class MissionEditorModel final : public EditorDocumentModel {
public:
    std::vector<MissionNodeModel> nodes;
    std::vector<MissionEdgeModel> edges;

    bool add_node(MissionNodeModel n) {
        if (n.id == 0 || std::any_of(nodes.begin(), nodes.end(),
                                     [&](const auto& x) { return x.id == n.id; })) {
            return false;
        }
        Snapshot before = snapshot();
        nodes.push_back(std::move(n));
        push_command("Add Mission Node", before, snapshot());
        return true;
    }

    bool connect(MissionEdgeModel e) {
        if (e.from == e.to || !has(e.from) || !has(e.to)) return false;
        if (std::any_of(edges.begin(), edges.end(),
                        [&](const auto& x) { return x.from == e.from && x.to == e.to; })) {
            return false;
        }
        Snapshot before = snapshot();
        edges.push_back(std::move(e));
        push_command("Connect Mission Nodes", before, snapshot());
        return true;
    }

    bool remove_node(uint64_t id) {
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const auto& n) { return n.id == id; });
        if (it == nodes.end()) return false;
        Snapshot before = snapshot();
        nodes.erase(it);
        edges.erase(std::remove_if(edges.begin(), edges.end(),
                                   [&](const auto& e) { return e.from == id || e.to == id; }),
                    edges.end());
        push_command("Remove Mission Node", before, snapshot());
        return true;
    }

    bool remove_edge(uint64_t from, uint64_t to) {
        const auto it = std::find_if(edges.begin(), edges.end(),
                                     [&](const auto& e) { return e.from == from && e.to == to; });
        if (it == edges.end()) return false;
        Snapshot before = snapshot();
        edges.erase(it);
        push_command("Remove Mission Edge", before, snapshot());
        return true;
    }

    bool move_node(uint64_t id, glm::vec2 position) {
        MissionNodeModel* node = find(id);
        if (!node) return false;
        Snapshot before = snapshot();
        node->position = position;
        push_command("Move Mission Node", before, snapshot());
        return true;
    }

    bool update_node(MissionNodeModel replacement) {
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const auto& n) { return n.id == replacement.id; });
        if (it == nodes.end()) return false;
        Snapshot before = snapshot();
        *it = std::move(replacement);
        push_command("Update Mission Node", before, snapshot());
        return true;
    }

    MissionNodeModel* find(uint64_t id) {
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const auto& n) { return n.id == id; });
        return it == nodes.end() ? nullptr : &*it;
    }
    const MissionNodeModel* find(uint64_t id) const {
        const auto it = std::find_if(nodes.begin(), nodes.end(),
                                     [&](const auto& n) { return n.id == id; });
        return it == nodes.end() ? nullptr : &*it;
    }

    bool has(uint64_t id) const { return find(id) != nullptr; }

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

        if (std::count_if(nodes.begin(), nodes.end(),
                          [](const auto& n) { return n.kind == MissionNodeKind::Entry; }) != 1) {
            errors("entry", "Mission requires exactly one entry node");
        }
        for (const auto& e : edges) {
            if (!has(e.from) || !has(e.to)) errors("edge", "Edge references a missing node");
            if (e.from == e.to) errors("edge", "Edge is a self-loop");
        }
        // Duplicate edges.
        for (std::size_t i = 0; i < edges.size(); ++i) {
            for (std::size_t j = i + 1; j < edges.size(); ++j) {
                if (edges[i].from == edges[j].from && edges[i].to == edges[j].to) {
                    errors("edge", "Duplicate edge between the same nodes");
                }
            }
        }
        // Cycles: missions must be acyclic (start -> ... -> complete).
        if (has_cycle()) errors("graph", "Mission graph contains a cycle");
        // Isolated nodes never participate in the flow.
        for (const auto& n : nodes) {
            const bool incident =
                std::any_of(edges.begin(), edges.end(),
                            [&](const auto& e) { return e.from == n.id || e.to == n.id; });
            if (!incident) {
                warnings(std::to_string(n.id), "Mission node is disconnected");
            }
        }
        return issues;
    }

private:
    struct Snapshot {
        std::vector<MissionNodeModel> nodes;
        std::vector<MissionEdgeModel> edges;
    };
    struct Command {
        std::string name;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    [[nodiscard]] Snapshot snapshot() const { return {nodes, edges}; }
    void restore(const Snapshot& state) {
        nodes = state.nodes;
        edges = state.edges;
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

    [[nodiscard]] bool has_cycle() const {
        std::unordered_map<uint64_t, std::vector<uint64_t>> adjacency;
        for (const auto& e : edges) adjacency[e.from].push_back(e.to);
        std::unordered_set<uint64_t> visited;
        std::unordered_set<uint64_t> inStack;
        std::function<bool(uint64_t)> dfs = [&](uint64_t id) -> bool {
            if (inStack.count(id) != 0) return true;
            if (visited.count(id) != 0) return false;
            visited.insert(id);
            inStack.insert(id);
            const auto it = adjacency.find(id);
            if (it != adjacency.end()) {
                for (const uint64_t next : it->second) {
                    if (dfs(next)) return true;
                }
            }
            inStack.erase(id);
            return false;
        };
        for (const auto& entry : adjacency) {
            if (dfs(entry.first)) return true;
        }
        return false;
    }

    std::vector<Command> undoStack_;
    std::vector<Command> redoStack_;
};

} // namespace Engine::Editor
