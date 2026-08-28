#include "engine/plugins/IPluginManifestCodec.hpp"

#include <sstream>

namespace engine::plugins {

std::string PluginManifest::to_json() const {
    std::ostringstream out;
    out << "{\"name\":\"" << name
        << "\",\"display_name\":\"" << display_name
        << "\",\"description\":\"" << description
        << "\",\"author\":\"" << author
        << "\",\"version\":\"" << version.to_string()
        << "\",\"abi\":" << static_cast<unsigned>(abi)
        << "}";
    return out.str();
}

PluginManifest PluginManifest::from_json(const std::string& input, std::string& error) {
    PluginManifest result;
    auto field = [&](const char* key) -> std::string {
        const std::string marker = std::string("\"") + key + "\":\"";
        const std::size_t begin = input.find(marker);
        if (begin == std::string::npos) return {};
        const std::size_t valueBegin = begin + marker.size();
        const std::size_t end = input.find('\"', valueBegin);
        return end == std::string::npos ? std::string{} : input.substr(valueBegin, end - valueBegin);
    };
    result.name = field("name");
    result.display_name = field("display_name");
    result.description = field("description");
    result.author = field("author");
    const std::string versionText = field("version");
    if (!versionText.empty()) result.version = PluginVersion::parse(versionText);
    if (!result.validate(error)) return {};
    error.clear();
    return result;
}

namespace { class Codec final : public IPluginManifestCodec {
public:
 bool encode(const PluginManifest& m,std::string& out,std::string& e) const override { if(!m.validate(e)) return false; out=m.to_json(); return true; }
 bool decode(const std::string& in,PluginManifest& out,std::string& e) const override { out=PluginManifest::from_json(in,e); return !out.name.empty(); }
}; }
std::unique_ptr<IPluginManifestCodec> create_plugin_manifest_codec(){return std::make_unique<Codec>();}
}
