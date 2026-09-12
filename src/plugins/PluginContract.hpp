#pragma once

#include "../engine/core/plugin/Plugin.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Engine::Editor { class EditorRegistry; }
namespace engine::plugins {
class IPluginLoader;
class IPluginPermissionPolicy;
class IPluginTypeRegistry;
}

namespace Engine::Plugins {

struct PluginContext {
    TypeRegistry& types;
    AssetRegistry& assets;
    Editor::EditorRegistry* editor{}; // Null in game/server builds.
    std::function<void(std::string_view)> unregisterEditorOwner;
};

// Runtime/editor-neutral plugin contract. Bind before passing the plugin to
// PluginRegistry so registration occurs during on_load().
class EnginePlugin : public Plugin {
public:
    void bind(PluginContext context);
    [[nodiscard]] bool is_bound() const noexcept { return context_ != nullptr; }
    [[nodiscard]] bool is_loaded() const noexcept { return loaded_; }
    void on_load() final;
    void on_unload() final;

protected:
    virtual void register_types(TypeRegistry&) = 0;
    virtual void register_assets(AssetRegistry&) = 0;
    virtual void register_editor_tools(Editor::EditorRegistry&) {}
    virtual void startup() {}
    virtual void shutdown() noexcept {}
    [[nodiscard]] PluginContext& context();
private:
    std::unique_ptr<PluginContext> context_;
    bool loaded_{};
};

void register_plugin_type(TypeRegistry& registry, std::string name);
void register_asset_tool(Editor::EditorRegistry& registry, std::string assetType,
                         std::string menuPath, std::string owner);
void register_viewport_tool(Editor::EditorRegistry& registry, std::string toolId,
                            std::string menuPath, std::string owner);

// Product accessors expose the canonical plugin services already owned by
// PluginContract.cpp. They do not allocate a second loader/policy/registry;
// editor/game diagnostics query the same instances that gate on_load/on_unload.
engine::plugins::IPluginLoader* product_plugin_loader() noexcept;
engine::plugins::IPluginPermissionPolicy* product_plugin_permissions() noexcept;
engine::plugins::IPluginTypeRegistry* product_plugin_type_registry() noexcept;

} // namespace Engine::Plugins
