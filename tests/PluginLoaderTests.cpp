#include "engine/plugins/IPluginLoader.hpp"
#include <cassert>
int main(){auto loader=engine::plugins::create_plugin_loader();std::string e;engine::plugins::PluginManifest m;m.name="com.example.mod";m.display_name="Example";engine::plugins::PluginLoadOptions o;o.permissions.granted={};assert(loader->register_plugin(m,o,e));assert(loader->load(m.name,e));assert(loader->is_loaded(m.name));auto result=loader->load_result(m.name,e);assert(result.loaded);assert(loader->unload(m.name,e));assert(!loader->is_loaded(m.name));assert(!loader->load("missing",e));return 0;}
