#pragma once
#include "engine/plugins/IPluginIsolation.hpp"
#include "engine/plugins/IPluginManifest.hpp"
#include "engine/plugins/IPluginSandbox.hpp"
#include "engine/plugins/IPluginTypeRegistry.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>
namespace engine::plugins {
struct PluginLoadOptions{std::string library_path;PluginPermissions permissions;std::uint64_t timeout_ms{0};std::uint64_t memory_limit_bytes{0};bool allow_dynamic_code{false};};
struct PluginLoadResult{bool loaded{false};std::string plugin_name;std::string error;PluginState state{PluginState::Unloaded};};
class IPluginLoader{public:virtual~IPluginLoader()=default;virtual bool register_plugin(const PluginManifest&,const PluginLoadOptions&,std::string&)=0;virtual bool load(const std::string&,std::string&)=0;virtual PluginLoadResult load_result(const std::string&,std::string&)=0;virtual bool unload(const std::string&,std::string&)=0;virtual bool reload(const std::string&,std::string&)=0;virtual PluginLoadResult reload_result(const std::string&,std::string&)=0;virtual bool is_loaded(const std::string&)const=0;virtual std::vector<PluginRuntimeInfo>runtime()const=0;virtual const IPluginSandbox*sandbox(const std::string&)const=0;virtual IPluginTypeRegistry&registry()=0;};
std::unique_ptr<IPluginLoader>create_plugin_loader();
}
