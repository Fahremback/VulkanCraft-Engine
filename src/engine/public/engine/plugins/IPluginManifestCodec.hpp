#pragma once
#include "engine/plugins/IPluginManifest.hpp"
#include <memory>
#include <string>
namespace engine::plugins {
class IPluginManifestCodec {
public:
    virtual ~IPluginManifestCodec() = default;
    virtual bool encode(const PluginManifest&, std::string&, std::string&) const = 0;
    virtual bool decode(const std::string&, PluginManifest&, std::string&) const = 0;
};
std::unique_ptr<IPluginManifestCodec> create_plugin_manifest_codec();
}
