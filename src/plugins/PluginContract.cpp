#include "PluginContract.hpp"

namespace Engine::Plugins {
void EnginePlugin::bind(PluginContext context) {
    if (loaded_) throw std::logic_error("Cannot rebind a loaded plugin");
    context_ = std::make_unique<PluginContext>(std::move(context));
}
PluginContext& EnginePlugin::context() {
    if (!context_) throw std::logic_error("Plugin context was not bound");
    return *context_;
}
void EnginePlugin::on_load() {
    if (loaded_) return;
    auto& ctx = context();
    register_types(ctx.types);
    register_assets(ctx.assets);
    if (ctx.editor) register_editor_tools(*ctx.editor);
    startup();
    loaded_ = true;
}
void EnginePlugin::on_unload() {
    if (!loaded_) return;
    shutdown();
    if (context_ && context_->unregisterEditorOwner)
        context_->unregisterEditorOwner(get_name());
    loaded_ = false;
}
void register_plugin_type(TypeRegistry& registry, std::string name) {
    ClassMetaData metadata;
    metadata.name = std::move(name);
    registry.register_class(metadata);
}
} // namespace Engine::Plugins
