#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::assets {

enum class AssetFormat : std::uint8_t { Gltf, Usd, MaterialX, Ktx, Text, Font };

struct FormatCapabilities {
    AssetFormat format{AssetFormat::Text};
    bool import_supported{false};
    bool export_supported{false};
    bool deterministic{true};
    std::string extension;
};

struct FormatDependency {
    std::string path;
    std::string kind;
};

struct FormatDocument {
    AssetFormat format{AssetFormat::Text};
    std::string source_name;
    std::vector<std::uint8_t> bytes;
    std::vector<FormatDependency> dependencies;
    std::uint64_t revision{0};
};

struct FormatValidation {
    bool valid{false};
    std::vector<std::string> errors;
    std::vector<FormatDependency> dependencies;
};

class IAssetFormatRegistry {
public:
    virtual ~IAssetFormatRegistry() = default;
    virtual std::vector<FormatCapabilities> capabilities() const = 0;
    virtual bool import_document(const FormatDocument&, std::string&) = 0;
    virtual bool reimport_document(const FormatDocument&, std::string&) = 0;
    virtual FormatValidation validate(AssetFormat) const = 0;
    virtual bool export_document(AssetFormat, std::vector<std::uint8_t>&, std::string&) const = 0;
    virtual const FormatDocument* document(AssetFormat) const = 0;
    virtual std::vector<FormatDependency> dependencies(AssetFormat) const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<IAssetFormatRegistry> create_asset_format_registry();

} // namespace engine::assets
