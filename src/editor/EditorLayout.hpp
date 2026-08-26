#pragma once

// EditorLayout (agente 2 §B): the shell's PERSISTENT layout model — which
// panels are visible, which panel is focused, the gizmo mode, and the
// viewport-first arrangement. Data-driven from the panel plugin registry
// (EditorPlugin.hpp): the DEFAULT layout is derived from each panel's
// default_open flag, so new plugins get a sensible default without editing
// the shell. Model-only and headless (no ImGui/GPU): the shell applies the
// snapshot to its real windows, and saves/loads it across sessions.
//   - RESET is SAFE: it always rebuilds from the registry defaults — never
//     leaves the shell with a corrupted/empty state, never depends on prior
//     state.
//   - LOAD is all-or-nothing for malformed input (never mutates), and
//     TOLERANT for unknown panel ids (a snapshot written by a newer editor
//     with more plugins keeps the known panels and ignores the unknown ones
//     instead of failing — forward compatibility).
//   - DETERMINISM: same registry + operations -> identical snapshots and
//     JSON, bit-exact.

#include <string>
#include <vector>

namespace Engine {
namespace Editor {

struct EditorPanelSpec;  // fwd (EditorPlugin.hpp)

// One panel's persisted visibility.
struct EditorPanelVisibility {
    std::string id;
    bool visible{ false };
};

// The full persisted shell layout, versioned and JSON round-trippable.
struct EditorLayoutSnapshot {
    int version{ 1 };
    bool viewport_first{ true };
    std::string active_panel;   // focused panel id ("" = none)
    std::string gizmo_mode;     // "select" | "translate" | "rotate" | "scale"
    std::vector<EditorPanelVisibility> panels;  // insertion order

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// The layout runtime. PURE state machine over panel visibility.
class EditorLayoutModel {
public:
    EditorLayoutModel() = default;

    // (Re)builds the layout from the registry defaults (safe reset). Every
    // registered panel appears; panels with default_open=true start visible.
    // Order = registry insertion order. Never fails.
    void reset_from_registry(const std::vector<EditorPanelSpec>& panels);

    // Toggles/forces visibility of a known panel id. Returns false (no
    // mutation) when the id is not in the model.
    bool set_visible(const std::string& panelId, bool visible);

    // True when the panel is in the model and visible.
    bool is_visible(const std::string& panelId) const;

    // Visible panel ids, model order.
    std::vector<std::string> visible_panels() const;

    // Panel ids known to the model (registry order).
    std::vector<std::string> panel_ids() const;

    void set_active_panel(const std::string& panelId);
    std::string active_panel() const;

    void set_gizmo_mode(const std::string& mode);
    std::string gizmo_mode() const;

    void set_viewport_first(bool first);
    bool viewport_first() const;

    // Serializes the current layout. Always valid.
    EditorLayoutSnapshot snapshot() const;

    // Applies a snapshot: known panel ids are applied, unknown ids are
    // ignored (forward compatibility). The active panel and gizmo mode are
    // applied only when non-empty. Returns false + errorOut (no mutation)
    // only for malformed snapshots (parse/version/validate failure).
    bool apply_snapshot(const EditorLayoutSnapshot& snapshot,
                        std::string& errorOut);

private:
    std::vector<EditorPanelVisibility> panels_;
    std::string active_panel_;
    std::string gizmo_mode_;
    bool viewport_first_{ true };
};

}  // namespace Editor
}  // namespace Engine
