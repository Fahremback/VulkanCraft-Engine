// EditorLayout.cpp — the ONLY TU with the shell layout persistence behavior
// (agente 2 §B). PURE model: panel visibility derived from the panel plugin
// registry defaults, an optional focused panel, gizmo mode, and the
// viewport-first flag. reset_from_registry() is a SAFE reset (never depends
// on prior state); apply_snapshot() is all-or-nothing for malformed input and
// tolerant of unknown panel ids (forward compatibility). No ImGui/GPU.

#include "EditorLayout.hpp"

#include "EditorPlugin.hpp"

#include "../engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <sstream>

namespace Engine {
namespace Editor {

namespace {

bool valid_gizmo_mode(const std::string& mode) {
    return mode.empty() || mode == "select" || mode == "translate" ||
           mode == "rotate" || mode == "scale";
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

}  // namespace

bool EditorLayoutSnapshot::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported layout snapshot version";
        return false;
    }
    if (!valid_gizmo_mode(gizmo_mode)) {
        errorOut = "unknown layout gizmo_mode: " + gizmo_mode;
        return false;
    }
    return true;
}

std::string EditorLayoutSnapshot::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << version
        << ",\"viewport_first\":" << (viewport_first ? "true" : "false")
        << ",\"active_panel\":\"" << json_escape(active_panel)
        << "\",\"gizmo_mode\":\"" << json_escape(gizmo_mode)
        << "\",\"panels\":[";
    for (std::size_t i = 0; i < panels.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":\"" << json_escape(panels[i].id)
            << "\",\"visible\":" << (panels[i].visible ? "true" : "false")
            << "}";
    }
    out << "]}";
    return out.str();
}

bool EditorLayoutSnapshot::load_from_json(const std::string& jsonText,
                                          std::string& errorOut) {
    engine::sdk::JsonValue doc;
    if (!engine::sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "layout snapshot document must be an object";
        return false;
    }
    const int version =
        static_cast<int>(engine::sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported layout snapshot version";
        return false;
    }

    EditorLayoutSnapshot candidate;
    candidate.version = version;
    candidate.viewport_first =
        engine::sdk::json_bool(doc, "viewport_first", true);
    candidate.active_panel = engine::sdk::json_string(doc, "active_panel", "");
    candidate.gizmo_mode = engine::sdk::json_string(doc, "gizmo_mode", "");
    if (const engine::sdk::JsonValue* arr = doc.field("panels")) {
        if (!arr->is_array()) {
            errorOut = "layout snapshot field 'panels' must be an array";
            return false;
        }
        for (const engine::sdk::JsonValue& entry : arr->array) {
            EditorPanelVisibility pv;
            pv.id = engine::sdk::json_string(entry, "id", "");
            pv.visible = engine::sdk::json_bool(entry, "visible", false);
            candidate.panels.push_back(std::move(pv));
        }
    }
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

void EditorLayoutModel::reset_from_registry(
    const std::vector<EditorPanelSpec>& panels) {
    panels_.clear();
    panels_.reserve(panels.size());
    for (const EditorPanelSpec& spec : panels) {
        EditorPanelVisibility pv;
        pv.id = spec.id;
        pv.visible = spec.default_open;
        panels_.push_back(std::move(pv));
    }
    active_panel_.clear();
    gizmo_mode_.clear();
    viewport_first_ = true;
}

bool EditorLayoutModel::set_visible(const std::string& panelId, bool visible) {
    for (EditorPanelVisibility& pv : panels_) {
        if (pv.id == panelId) {
            pv.visible = visible;
            if (!visible && active_panel_ == panelId) active_panel_.clear();
            return true;
        }
    }
    return false;
}

bool EditorLayoutModel::is_visible(const std::string& panelId) const {
    for (const EditorPanelVisibility& pv : panels_) {
        if (pv.id == panelId) return pv.visible;
    }
    return false;
}

std::vector<std::string> EditorLayoutModel::visible_panels() const {
    std::vector<std::string> out;
    for (const EditorPanelVisibility& pv : panels_) {
        if (pv.visible) out.push_back(pv.id);
    }
    return out;
}

std::vector<std::string> EditorLayoutModel::panel_ids() const {
    std::vector<std::string> out;
    out.reserve(panels_.size());
    for (const EditorPanelVisibility& pv : panels_) out.push_back(pv.id);
    return out;
}

void EditorLayoutModel::set_active_panel(const std::string& panelId) {
    active_panel_ = panelId;
}

std::string EditorLayoutModel::active_panel() const { return active_panel_; }

void EditorLayoutModel::set_gizmo_mode(const std::string& mode) {
    if (valid_gizmo_mode(mode)) gizmo_mode_ = mode;
}

std::string EditorLayoutModel::gizmo_mode() const { return gizmo_mode_; }

void EditorLayoutModel::set_viewport_first(bool first) { viewport_first_ = first; }

bool EditorLayoutModel::viewport_first() const { return viewport_first_; }

EditorLayoutSnapshot EditorLayoutModel::snapshot() const {
    EditorLayoutSnapshot snap;
    snap.version = 1;
    snap.viewport_first = viewport_first_;
    snap.active_panel = active_panel_;
    snap.gizmo_mode = gizmo_mode_;
    snap.panels = panels_;
    return snap;
}

bool EditorLayoutModel::apply_snapshot(const EditorLayoutSnapshot& snapshot,
                                       std::string& errorOut) {
    errorOut.clear();
    if (!snapshot.validate(errorOut)) return false;

    // Apply to a copy so unknown ids can be ignored without corrupting state.
    EditorLayoutModel candidate = *this;
    candidate.viewport_first_ = snapshot.viewport_first;

    // Known id -> apply visibility; unknown id -> ignore (forward compat).
    for (const EditorPanelVisibility& pv : snapshot.panels) {
        if (pv.id.empty()) continue;
        const bool known =
            std::any_of(candidate.panels_.begin(), candidate.panels_.end(),
                        [&](const EditorPanelVisibility& existing) {
                            return existing.id == pv.id;
                        });
        if (!known) continue;
        candidate.set_visible(pv.id, pv.visible);
    }

    if (!snapshot.active_panel.empty()) {
        candidate.active_panel_ = snapshot.active_panel;
    }
    if (!snapshot.gizmo_mode.empty()) {
        candidate.gizmo_mode_ = snapshot.gizmo_mode;
    }

    *this = std::move(candidate);
    return true;
}

}  // namespace Editor
}  // namespace Engine