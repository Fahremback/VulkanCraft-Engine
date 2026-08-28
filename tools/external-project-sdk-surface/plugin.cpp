#include <engine/plugins/IPluginLoader.hpp>
#include <engine/plugins/IPluginPermissions.hpp>
#include <engine/plugins/IPluginTypeRegistry.hpp>

extern "C" bool vulkan_craft_plugin_register(engine::plugins::IPluginTypeRegistry& registry,
                                               const char* plugin_name,
                                               std::string& error) {
    if (!plugin_name || !*plugin_name) { error = "plugin_name_required"; return false; }
    return registry.register_item({engine::plugins::PluginRegistrationKind::Type,
        "example.external.type", "External Type", "SDK plugin example", plugin_name, "1.0.0"}, &error);
}
