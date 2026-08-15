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

} // namespace Engine::Plugins
