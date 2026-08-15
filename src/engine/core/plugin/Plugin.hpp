#pragma once

#include <string>
#include <vector>
#include <memory>
#include "../reflection/Reflection.hpp"
#include "../../assets/AssetRegistry.hpp"

namespace Engine {

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual std::string get_name() const = 0;
    virtual std::string get_version() const = 0;

    virtual void on_load() = 0;
    virtual void on_unload() = 0;
};

class PluginRegistry {
public:
    static PluginRegistry& get() {
        static PluginRegistry inst;
        return inst;
    }

    void register_plugin(std::shared_ptr<Plugin> plugin) {
        if (!plugin) return;
        std::string name = plugin->get_name();
        if (find_plugin(name)) return;
        plugin->on_load();
        m_enabled[name] = true;
        m_plugins.push_back(plugin);
    }

    bool is_plugin_enabled(const std::string& name) const {
        auto it = m_enabled.find(name);
        if (it != m_enabled.end()) return it->second;
        return false;
    }

    bool set_plugin_enabled(const std::string& name, bool enabled) {
        auto plugin = find_plugin(name);
        if (!plugin) return false;
        bool current = is_plugin_enabled(name);
        if (current == enabled) return true;
        if (enabled) {
            plugin->on_load();
            m_enabled[name] = true;
        } else {
            plugin->on_unload();
            m_enabled[name] = false;
        }
        return true;
    }

    std::shared_ptr<Plugin> find_plugin(const std::string& name) const {
        for (const auto& plugin : m_plugins) {
            if (plugin->get_name() == name) return plugin;
        }
        return nullptr;
    }

    void unload_all() {
        for (auto& plugin : m_plugins) {
            if (is_plugin_enabled(plugin->get_name())) {
                plugin->on_unload();
            }
        }
        m_plugins.clear();
        m_enabled.clear();
    }

    const std::vector<std::shared_ptr<Plugin>>& get_plugins() const {
        return m_plugins;
    }

private:
    std::vector<std::shared_ptr<Plugin>> m_plugins;
    std::unordered_map<std::string, bool> m_enabled;
};

} // namespace Engine
