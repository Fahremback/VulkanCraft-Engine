#include "engine/assets/IAssetFormats.hpp"
#include <algorithm>

namespace engine::assets {
namespace {
class Registry final : public IAssetFormatRegistry {
public:
    Registry() {
        for (auto format : {AssetFormat::Gltf, AssetFormat::Usd, AssetFormat::MaterialX,
                            AssetFormat::Ktx, AssetFormat::Text, AssetFormat::Font})
            caps_.push_back({format, true, true, true});
    }
    std::vector<FormatCapabilities> capabilities() const override { return caps_; }
    bool import_document(const FormatDocument& doc, std::string& error) override {
        if (doc.source_name.empty() || doc.bytes.empty()) { error = "invalid_document"; return false; }
        auto it = std::find_if(docs_.begin(), docs_.end(), [&](const auto& d) { return d.format == doc.format; });
        if (it == docs_.end()) docs_.push_back(doc); else *it = doc;
        return true;
    }
    bool export_document(AssetFormat format, std::vector<std::uint8_t>& bytes, std::string& error) const override {
        const auto* doc = document(format);
        if (!doc) { error = "document_not_found"; return false; }
        bytes = doc->bytes;
        return true;
    }
    const FormatDocument* document(AssetFormat format) const override {
        auto it = std::find_if(docs_.begin(), docs_.end(), [&](const auto& d) { return d.format == format; });
        return it == docs_.end() ? nullptr : &*it;
    }
    void clear() override { docs_.clear(); }
private:
    std::vector<FormatCapabilities> caps_;
    std::vector<FormatDocument> docs_;
};
}
std::unique_ptr<IAssetFormatRegistry> create_asset_format_registry() { return std::make_unique<Registry>(); }
}
