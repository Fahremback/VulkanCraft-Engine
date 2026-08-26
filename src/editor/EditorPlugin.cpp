#include "EditorPlugin.hpp"

#include <algorithm>

namespace Engine::Editor {

bool EditorPluginRegistry::register_panel(EditorPanelSpec spec) {
    if (spec.id.empty()) return false;
    if (by_id_.find(spec.id) != by_id_.end()) return false;
    order_.push_back(spec.id);
    by_id_.emplace(spec.id, std::move(spec));
    return true;
}

bool EditorPluginRegistry::unregister_panel(const std::string& id) {
    const auto it = by_id_.find(id);
    if (it == by_id_.end()) return false;
    by_id_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    return true;
}

bool EditorPluginRegistry::contains(const std::string& id) const noexcept {
    return by_id_.find(id) != by_id_.end();
}

const EditorPanelSpec* EditorPluginRegistry::find(const std::string& id) const noexcept {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &it->second;
}

std::size_t EditorPluginRegistry::size() const noexcept {
    return by_id_.size();
}

std::vector<EditorPanelSpec> EditorPluginRegistry::panels() const {
    std::vector<EditorPanelSpec> result;
    result.reserve(order_.size());
    for (const std::string& id : order_) {
        result.push_back(by_id_.at(id));
    }
    return result;
}

std::vector<std::string> EditorPluginRegistry::panel_ids() const {
    return order_;
}

std::vector<EditorPanelSpec> EditorPluginRegistry::panels_in_category(const std::string& category) const {
    std::vector<EditorPanelSpec> result;
    for (const std::string& id : order_) {
        const EditorPanelSpec& spec = by_id_.at(id);
        if (spec.category == category) result.push_back(spec);
    }
    return result;
}

std::vector<std::string> EditorPluginRegistry::categories() const {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (const std::string& id : order_) {
        const EditorPanelSpec& spec = by_id_.at(id);
        if (seen.insert(spec.category).second) result.push_back(spec.category);
    }
    return result;
}

void EditorPluginRegistry::clear() noexcept {
    by_id_.clear();
    order_.clear();
}

} // namespace Engine::Editor