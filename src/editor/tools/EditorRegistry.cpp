#include "EditorRegistry.hpp"
#include <algorithm>

namespace Engine::Editor {

bool EditorRegistry::register_component_editor(EditorTypeId type, ComponentEditorFactory factory, EditorOwnerId owner) {
    if (type.empty() || !factory || owner.empty()) return false;
    return componentEditors_.emplace(std::move(type), Registration<ComponentEditorFactory>{std::move(factory), std::move(owner)}).second;
}
bool EditorRegistry::register_asset_editor(EditorTypeId type, AssetEditorFactory factory, EditorOwnerId owner) {
    if (type.empty() || !factory || owner.empty()) return false;
    return assetEditors_.emplace(std::move(type), Registration<AssetEditorFactory>{std::move(factory), std::move(owner)}).second;
}
bool EditorRegistry::register_menu_item(MenuItemDesc item, EditorOwnerId owner) {
    if (item.id.empty() || item.path.empty() || item.label.empty() || !item.execute || owner.empty()) return false;
    return menuItems_.emplace(item.id, MenuRegistration{std::move(item), std::move(owner)}).second;
}
bool EditorRegistry::register_viewport_tool(std::string id, ViewportToolFactory factory, EditorOwnerId owner) {
    if (id.empty() || !factory || owner.empty()) return false;
    return viewportTools_.emplace(std::move(id), Registration<ViewportToolFactory>{std::move(factory), std::move(owner)}).second;
}

std::unique_ptr<IComponentEditor> EditorRegistry::create_component_editor(std::string_view type) const {
    const auto it = componentEditors_.find(std::string(type)); return it == componentEditors_.end() ? nullptr : it->second.factory();
}
std::unique_ptr<IAssetEditor> EditorRegistry::create_asset_editor(std::string_view type) const {
    const auto it = assetEditors_.find(std::string(type)); return it == assetEditors_.end() ? nullptr : it->second.factory();
}
std::unique_ptr<IViewportTool> EditorRegistry::create_viewport_tool(std::string_view id) const {
    const auto it = viewportTools_.find(std::string(id)); return it == viewportTools_.end() ? nullptr : it->second.factory();
}
std::vector<MenuItemDesc> EditorRegistry::menu_items(std::string_view prefix) const {
    std::vector<MenuItemDesc> result;
    for (const auto& [_, registration] : menuItems_)
        if (prefix.empty() || registration.item.path.starts_with(prefix)) result.push_back(registration.item);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.path == b.path ? (a.order == b.order ? a.label < b.label : a.order < b.order) : a.path < b.path;
    });
    return result;
}
bool EditorRegistry::execute_menu_item(std::string_view id) const {
    const auto it = menuItems_.find(std::string(id));
    if (it == menuItems_.end() || (it->second.item.enabled && !it->second.item.enabled())) return false;
    it->second.item.execute(); return true;
}
std::vector<EditorExtensionInfo> EditorRegistry::extensions() const {
    std::vector<EditorExtensionInfo> result;
    for (const auto& [id, value] : componentEditors_) result.push_back({"component:" + id, value.owner});
    for (const auto& [id, value] : assetEditors_) result.push_back({"asset:" + id, value.owner});
    for (const auto& [id, value] : viewportTools_) result.push_back({"viewport:" + id, value.owner});
    for (const auto& [id, value] : menuItems_) result.push_back({"menu:" + id, value.owner});
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}
size_t EditorRegistry::unregister_owner(std::string_view owner) {
    size_t count{};
    count += std::erase_if(componentEditors_, [owner](const auto& p) { return p.second.owner == owner; });
    count += std::erase_if(assetEditors_, [owner](const auto& p) { return p.second.owner == owner; });
    count += std::erase_if(viewportTools_, [owner](const auto& p) { return p.second.owner == owner; });
    count += std::erase_if(menuItems_, [owner](const auto& p) { return p.second.owner == owner; });
    return count;
}
void EditorRegistry::clear() { componentEditors_.clear(); assetEditors_.clear(); viewportTools_.clear(); menuItems_.clear(); }

} // namespace Engine::Editor
