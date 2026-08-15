#pragma once

#include "../../engine/core/uuid/UUID.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Engine::Editor {

using EditorTypeId = std::string;
using EditorOwnerId = std::string;

class IComponentEditor {
public:
    virtual ~IComponentEditor() = default;
    virtual void set_target(void* component) = 0;
};

class IAssetEditor {
public:
    virtual ~IAssetEditor() = default;
    virtual void open_asset(UUID asset) = 0;
};

class IViewportTool {
public:
    virtual ~IViewportTool() = default;
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    virtual void activate() {}
    virtual void deactivate() {}
};

using ComponentEditorFactory = std::function<std::unique_ptr<IComponentEditor>()>;
using AssetEditorFactory = std::function<std::unique_ptr<IAssetEditor>()>;
using ViewportToolFactory = std::function<std::unique_ptr<IViewportTool>()>;

struct MenuItemDesc {
    std::string id;
    std::string path;
    std::string label;
    std::string shortcut;
    int32_t order{};
    std::function<bool()> enabled;
    std::function<void()> execute;
};

struct EditorExtensionInfo {
    std::string id;
    EditorOwnerId owner;
};

class EditorRegistry final {
public:
    [[nodiscard]] bool register_component_editor(EditorTypeId componentType, ComponentEditorFactory factory,
                                                 EditorOwnerId owner = "core");
    [[nodiscard]] bool register_asset_editor(EditorTypeId assetType, AssetEditorFactory factory,
                                             EditorOwnerId owner = "core");
    [[nodiscard]] bool register_menu_item(MenuItemDesc item, EditorOwnerId owner = "core");
    [[nodiscard]] bool register_viewport_tool(std::string id, ViewportToolFactory factory,
                                             EditorOwnerId owner = "core");

    [[nodiscard]] std::unique_ptr<IComponentEditor> create_component_editor(std::string_view componentType) const;
    [[nodiscard]] std::unique_ptr<IAssetEditor> create_asset_editor(std::string_view assetType) const;
    [[nodiscard]] std::unique_ptr<IViewportTool> create_viewport_tool(std::string_view id) const;
    [[nodiscard]] std::vector<MenuItemDesc> menu_items(std::string_view pathPrefix = {}) const;
    [[nodiscard]] bool execute_menu_item(std::string_view id) const;
    [[nodiscard]] std::vector<EditorExtensionInfo> extensions() const;

    // Removes every extension contributed by a plugin before its library is unloaded.
    size_t unregister_owner(std::string_view owner);
    void clear();

private:
    template<class Factory> struct Registration { Factory factory; EditorOwnerId owner; };
    struct MenuRegistration { MenuItemDesc item; EditorOwnerId owner; };
    std::unordered_map<EditorTypeId, Registration<ComponentEditorFactory>> componentEditors_;
    std::unordered_map<EditorTypeId, Registration<AssetEditorFactory>> assetEditors_;
    std::unordered_map<std::string, Registration<ViewportToolFactory>> viewportTools_;
    std::unordered_map<std::string, MenuRegistration> menuItems_;
};

} // namespace Engine::Editor
