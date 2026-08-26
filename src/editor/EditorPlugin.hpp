#pragma once

// EditorPlugin — UI-independent registry for editor panels/plugins.
//
// This is the "tools as replaceable plugins" pillar absorbed from ezEngine:
// panels register a stable id + metadata, and the editor shell builds the
// View menu, the Ctrl+K command palette and layout persistence from the
// registry instead of hardcoding every window (the current monolithic
// WickedToolsPanel / m_show* flags pattern). Model-only: no ImGui, no GPU,
// fully headless-testable, additive to the existing editor.

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine::Editor {

/// Metadata for one editor panel/plugin. UI-independent: the shell decides
/// how to render `title`/`category` (toolbar, menus, palette).
struct EditorPanelSpec {
    std::string id;         ///< stable, unique id (e.g. "hierarchy", "content_browser")
    std::string title;      ///< display name (localization is the shell's job)
    std::string category;   ///< menu grouping (e.g. "scene", "assets", "animation")
    bool toggleable{true};  ///< shows a show/hide toggle in the View menu
    bool default_open{false}; ///< initial visibility for a fresh layout
};

/// Owns the set of panels the editor can host. Insertion order is stable so
/// menus/layout are deterministic; registration is all-or-nothing (empty or
/// duplicate ids are refused without mutating the registry).
class EditorPluginRegistry {
public:
    /// Registers a panel. Returns false (registry unchanged) when `id` is
    /// empty or already registered — never silently overwrites.
    bool register_panel(EditorPanelSpec spec);

    /// Removes a panel by id. Returns false for unknown ids.
    bool unregister_panel(const std::string& id);

    [[nodiscard]] bool contains(const std::string& id) const noexcept;
    [[nodiscard]] const EditorPanelSpec* find(const std::string& id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    /// All panels in insertion order.
    [[nodiscard]] std::vector<EditorPanelSpec> panels() const;
    /// Just the ids, in insertion order (cheap shell/telemetry surface).
    [[nodiscard]] std::vector<std::string> panel_ids() const;
    /// Panels of one category, insertion order.
    [[nodiscard]] std::vector<EditorPanelSpec> panels_in_category(const std::string& category) const;
    /// Distinct categories, first-seen order.
    [[nodiscard]] std::vector<std::string> categories() const;

    void clear() noexcept;

private:
    std::unordered_map<std::string, EditorPanelSpec> by_id_;
    std::vector<std::string> order_; // stable insertion order of ids
};

} // namespace Engine::Editor