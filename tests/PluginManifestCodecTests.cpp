#include "engine/plugins/IPluginManifestCodec.hpp"
#include <cassert>
#include <iostream>
int main(){auto c=engine::plugins::create_plugin_manifest_codec();engine::plugins::PluginManifest m;m.name="example.plugin";m.display_name="Example";m.author="SDK";m.version={1,2,3};std::string json,e;assert(c->encode(m,json,e));engine::plugins::PluginManifest copy;assert(c->decode(json,copy,e));assert(copy.name==m.name&&copy.version==m.version);std::cout<<"plugin-manifest-codec-ok\n";}
