#include "engine/assets/IAssetFormats.hpp"

#include <algorithm>
#include <utility>

namespace engine::assets {
namespace {

class Registry final : public IAssetFormatRegistry {
    std::vector<FormatDocument> documents_;

    static bool known(AssetFormat format) noexcept {
        return static_cast<std::uint8_t>(format) <= static_cast<std::uint8_t>(AssetFormat::Font);
    }

    static bool valid(const FormatDocument& document, std::string& error) {
        if (!known(document.format)) {
            error = "unsupported_format";
            return false;
        }
        if (document.source_name.empty()) {
            error = "empty_source_name";
            return false;
        }
        if (document.bytes.empty()) {
            error = "empty_document";
            return false;
        }
        for (const auto& dependency : document.dependencies) {
            if (dependency.path.empty() || dependency.kind.empty()) {
                error = "invalid_dependency";
                return false;
            }
        }
        return true;
    }

public:
    std::vector<FormatCapabilities> capabilities() const override {
        return {
            {AssetFormat::Gltf, true, true, true, ".gltf/.glb"},
            {AssetFormat::Usd, true, true, true, ".usd/.usda/.usdc"},
            {AssetFormat::MaterialX, true, true, true, ".mtlx"},
            {AssetFormat::Ktx, true, true, true, ".ktx/.ktx2"},
            {AssetFormat::Text, true, true, true, ".txt"},
            {AssetFormat::Font, true, true, true, ".ttf/.otf"}
        };
    }

    bool import_document(const FormatDocument& document, std::string& error) override {
        if (!valid(document, error)) return false;
        const auto it = std::find_if(documents_.begin(), documents_.end(), [&](const auto& item) {
            return item.format == document.format;
        });
        if (it != documents_.end()) {
            error = "document_exists_use_reimport";
            return false;
        }
        auto copy = document;
        copy.revision = 1;
        documents_.push_back(std::move(copy));
        return true;
    }

    bool reimport_document(const FormatDocument& document, std::string& error) override {
        if (!valid(document, error)) return false;
        const auto it = std::find_if(documents_.begin(), documents_.end(), [&](const auto& item) {
            return item.format == document.format;
        });
        if (it == documents_.end()) {
            error = "document_not_found";
            return false;
        }
        auto copy = document;
        copy.revision = it->revision + 1;
        *it = std::move(copy);
        return true;
    }

    FormatValidation validate(AssetFormat format) const override {
        FormatValidation result;
        const auto* item = document(format);
        if (!item) {
            result.errors.push_back("document_not_found");
            return result;
        }
        std::string error;
        if (!valid(*item, error)) {
            result.errors.push_back(error);
            return result;
        }
        result.valid = true;
        result.dependencies = item->dependencies;
        return result;
    }

    bool export_document(AssetFormat format, std::vector<std::uint8_t>& bytes, std::string& error) const override {
        const auto* item = document(format);
        if (!item) {
            error = "document_not_found";
            return false;
        }
        bytes = item->bytes;
        return true;
    }

    const FormatDocument* document(AssetFormat format) const override {
        const auto it = std::find_if(documents_.begin(), documents_.end(), [&](const auto& item) {
            return item.format == format;
        });
        return it == documents_.end() ? nullptr : &*it;
    }

    std::vector<FormatDependency> dependencies(AssetFormat format) const override {
        const auto* item = document(format);
        return item ? item->dependencies : std::vector<FormatDependency>{};
    }

    void clear() override { documents_.clear(); }
};

} // namespace

std::unique_ptr<IAssetFormatRegistry> create_asset_format_registry() {
    return std::make_unique<Registry>();
}

} // namespace engine::assets
