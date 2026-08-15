#pragma once

// VisualScriptCanvas — a UI-independent model for visually editing a
// VisualScriptGraph on a 2D canvas. Owns nothing graphical: it tracks node
// layout (positions/sizes), viewport zoom/pan, selection (single, additive and
// marquee), copy/paste with UUID remapping, comment groups, title search,
// typed pin connection validation, graph validation (dangling pins, duplicate
// connections, type mismatches, disconnected nodes, pending pins, cycles) and
// a snapshot-based undo/redo stack with batching. Everything is testable
// without any UI.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "../core/uuid/UUID.hpp"
#include "VisualScriptGraph.hpp"

namespace Engine {

// Axis-aligned rectangle in canvas (world) space.
struct CanvasRect {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};

    [[nodiscard]] bool contains(const glm::vec2& point) const noexcept {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
    }
    [[nodiscard]] bool overlaps(const CanvasRect& other) const noexcept {
        return min.x <= other.max.x && other.min.x <= max.x &&
               min.y <= other.max.y && other.min.y <= max.y;
    }
    [[nodiscard]] CanvasRect expanded_by(float amount) const noexcept {
        return {{min.x - amount, min.y - amount}, {max.x + amount, max.y + amount}};
    }
    [[nodiscard]] bool valid() const noexcept { return min.x <= max.x && min.y <= max.y; }
};

// A single validation finding for a canvas graph.
struct CanvasIssue {
    enum class Severity : uint8_t { Info, Warning, Error };
    Severity severity{Severity::Info};
    std::string field;
    std::string message;
};

// Per-node canvas layout (position/size). The semantic node lives in
// VisualScriptGraph; this record only describes where it is drawn.
struct CanvasNodeLayout {
    UUID nodeID;
    glm::vec2 position{0.0f};
    glm::vec2 size{160.0f, 90.0f};
};

// A comment/group box that visually wraps nodes. Members are explicit node
// ids; an empty member list means the group is purely a comment.
struct CanvasGroup {
    UUID id;
    std::string title{"Group"};
    glm::vec2 position{0.0f};
    glm::vec2 size{240.0f, 140.0f};
    std::vector<UUID> members;

    [[nodiscard]] bool contains(UUID nodeID) const {
        for (const UUID& id : members) {
            if (id == nodeID) return true;
        }
        return false;
    }
};

// The payload captured by a copy operation: a subset of graph nodes plus the
// connections whose both endpoints belong to that subset.
struct CanvasClipboard {
    std::vector<ScriptNode> nodes;
    std::vector<ScriptConnection> connections;
    glm::vec2 origin{0.0f};

    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }
};

class VisualScriptCanvas {
public:
    VisualScriptCanvas() = default;
    explicit VisualScriptCanvas(VisualScriptGraph graph) : graph_(std::move(graph)) {}

    // --- semantic graph access ---
    [[nodiscard]] VisualScriptGraph& graph() noexcept { return graph_; }
    [[nodiscard]] const VisualScriptGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] const std::vector<ScriptNode>& nodes() const noexcept { return graph_.nodes; }
    [[nodiscard]] const std::vector<ScriptConnection>& connections() const noexcept {
        return graph_.connections;
    }
    ScriptNode* find_node(UUID id) noexcept;
    const ScriptNode* find_node(UUID id) const noexcept;
    const ScriptPin* find_pin_anywhere(UUID pinID, UUID* ownerNode = nullptr) const noexcept;
    [[nodiscard]] UUID owner_node(UUID pinID) const noexcept;

    // --- editing (every mutation is undoable and marks the canvas dirty) ---
    // Adds a node at the given position. If `node.id` is invalid a new UUID is
    // generated. Returns the node id, or an invalid UUID on failure.
    UUID add_node(const ScriptNode& node, glm::vec2 position);
    bool remove_node(UUID id);                       // also removes its pins' connections, layout, groups, selection
    bool move_node(UUID id, glm::vec2 position);
    bool move_selection(glm::vec2 delta);
    bool set_node_title(UUID id, std::string title);

    UUID add_pin(UUID nodeID, const ScriptPin& pin);
    bool remove_pin(UUID nodeID, UUID pinID);        // drops connections touching the pin

    // --- typed connections ---
    // Validates direction (output -> input), pin type equality, self/same-node
    // connections, duplicates and single-incoming-input. On failure returns
    // false and, if `reason` is non-null, writes a human-readable message.
    bool can_connect(UUID fromPin, UUID toPin, std::string* reason = nullptr) const;
    bool connect(UUID fromPin, UUID toPin, std::string* reason = nullptr);
    bool disconnect(UUID fromPin, UUID toPin);

    // --- viewport (zoom/pan; not undoable, does not mark dirty) ---
    void set_zoom(float zoom);
    void pan_by(glm::vec2 screenDelta) { viewportOffset_ += screenDelta; }
    void set_viewport_offset(glm::vec2 offset) { viewportOffset_ = offset; }
    [[nodiscard]] float zoom() const noexcept { return zoom_; }
    [[nodiscard]] glm::vec2 viewport_offset() const noexcept { return viewportOffset_; }
    [[nodiscard]] glm::vec2 screen_to_world(glm::vec2 screen) const noexcept;
    [[nodiscard]] glm::vec2 world_to_screen(glm::vec2 world) const noexcept;

    // --- selection (not undoable, does not mark dirty) ---
    void clear_selection() noexcept { selection_.clear(); }
    void select(UUID nodeID, bool additive = false);
    void deselect(UUID nodeID) noexcept;
    [[nodiscard]] bool is_selected(UUID nodeID) const noexcept;
    [[nodiscard]] const std::vector<UUID>& selection() const noexcept { return selection_; }
    [[nodiscard]] std::size_t selection_count() const noexcept { return selection_.size(); }

    // Marquee selection: rectangle given in *screen* coordinates.
    void begin_marquee(glm::vec2 screenStart) {
        marqueeStart_ = screenStart;
        marqueeEnd_ = screenStart;
    }
    void update_marquee(glm::vec2 screenEnd) { marqueeEnd_ = screenEnd; }
    // Commits the marquee; returns the node ids it hit. `additive` merges into
    // the existing selection instead of replacing it.
    std::vector<UUID> end_marquee(bool additive = false);

    // --- picking / hit testing (world coordinates) ---
    [[nodiscard]] CanvasRect node_rect(UUID id) const noexcept;
    [[nodiscard]] UUID node_at(glm::vec2 worldPos) const noexcept;
    [[nodiscard]] glm::vec2 pin_world_position(UUID nodeID, UUID pinID) const noexcept;

    // --- copy / paste ---
    [[nodiscard]] CanvasClipboard copy_selection() const;
    // Pastes a clipboard, remapping every node and pin UUID. The cluster keeps
    // its relative layout and is placed so its origin lands on `target`
    // (or the viewport center when `useViewportCenter` is true). Returns the
    // ids of the newly created nodes; the pasted nodes end up selected.
    std::vector<UUID> paste(const CanvasClipboard& clip, glm::vec2 target = glm::vec2(0.0f),
                            bool useViewportCenter = true);
    // Duplicates the current selection with an offset (copy + paste in place).
    std::vector<UUID> duplicate_selection(glm::vec2 offset = glm::vec2(40.0f, 40.0f));

    // --- groups / comments ---
    UUID add_group(std::string title, glm::vec2 position, glm::vec2 size);
    bool remove_group(UUID id);
    bool add_to_group(UUID groupID, UUID nodeID);
    bool remove_from_group(UUID groupID, UUID nodeID);
    // Creates a group that wraps the current selection (sized to its bounds).
    UUID group_selection(std::string title);
    [[nodiscard]] std::vector<UUID> groups_containing(UUID nodeID) const;
    [[nodiscard]] const std::vector<CanvasGroup>& groups() const noexcept { return groups_; }
    CanvasGroup* find_group(UUID id) noexcept;
    const CanvasGroup* find_group(UUID id) const noexcept;

    // --- search by title (case-insensitive substring) ---
    [[nodiscard]] std::vector<UUID> find_nodes(const std::string& query) const;

    // --- graph validation ---
    // Reports: dangling pin references, duplicate connections, type mismatches,
    // multiple inputs driving one input pin (all Errors), execution/value
    // cycles (Error), disconnected nodes (Warning) and input pins with no
    // incoming connection ("pending pins", Warning).
    [[nodiscard]] std::vector<CanvasIssue> validate() const;

    // --- undo / redo ---
    void undo();
    void redo();
    [[nodiscard]] bool can_undo() const noexcept { return !undoStack_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redoStack_.empty(); }
    [[nodiscard]] std::size_t undo_depth() const noexcept { return undoStack_.size(); }
    [[nodiscard]] std::size_t redo_depth() const noexcept { return redoStack_.size(); }
    void clear_undo() noexcept {
        undoStack_.clear();
        redoStack_.clear();
        batchEntries_.clear();
        batchDepth_ = 0;
    }
    // Merges all mutations between begin/end into a single undo step.
    void begin_batch(std::string name);
    void end_batch();

    // --- dirty tracking ---
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
    void mark_saved() noexcept { dirty_ = false; }

    // --- persistence (JSON) ---
    bool save_to_file(const std::filesystem::path& path) const;
    bool load_from_file(const std::filesystem::path& path);
    // In-memory JSON round-trip; load_from_file delegates here. Useful for
    // tests and for editor asset import without touching the filesystem.
    bool load_from_json_text(const std::string& document);

    // --- layout access ---
    [[nodiscard]] const CanvasNodeLayout* layout(UUID nodeID) const noexcept;
    CanvasNodeLayout* layout(UUID nodeID) noexcept;

private:
    // Snapshot-based undo: a full deep copy of the editable state.
    struct Snapshot {
        VisualScriptGraph graph;
        std::vector<CanvasNodeLayout> layouts;
        std::vector<CanvasGroup> groups;
    };
    struct Command {
        std::string name;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot& state);
    void push_command(std::string name, Snapshot before, Snapshot after);
    void changed() noexcept {
        dirty_ = true;
        ++revision_;
    }

    // Internal mutations without undo bookkeeping (used by paste/batches).
    UUID insert_node(const ScriptNode& node, glm::vec2 position);
    void insert_connection(const ScriptConnection& connection);

    VisualScriptGraph graph_;
    std::unordered_map<UUID, CanvasNodeLayout> layouts_;
    std::vector<CanvasGroup> groups_;

    float zoom_{1.0f};
    glm::vec2 viewportOffset_{0.0f};

    std::vector<UUID> selection_;
    std::optional<glm::vec2> marqueeStart_;
    std::optional<glm::vec2> marqueeEnd_;

    std::vector<Command> undoStack_;
    std::vector<Command> redoStack_;
    int batchDepth_{0};
    std::string batchName_;
    std::vector<std::pair<Snapshot, Snapshot>> batchEntries_;

    bool dirty_{false};
    uint64_t revision_{0};
};

} // namespace Engine
