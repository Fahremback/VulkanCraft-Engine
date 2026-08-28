#include "engine/assets/IAssetFormats.hpp"
#include <cassert>

int main() {
    using namespace engine::assets;
    auto registry = create_asset_format_registry();
    assert(registry->capabilities().size() == 6);
    std::string error;
    for (auto format : {AssetFormat::Gltf, AssetFormat::Usd, AssetFormat::MaterialX,
                        AssetFormat::Ktx, AssetFormat::Text, AssetFormat::Font}) {
        FormatDocument doc{format, "asset", {1, 2, 3}};
        assert(registry->import_document(doc, error));
        std::vector<std::uint8_t> output;
        assert(registry->export_document(format, output, error));
        assert(output == doc.bytes);
    }
    assert(!registry->import_document({AssetFormat::Text, "", {1}}, error));
    std::vector<std::uint8_t> missing;
    registry->clear();
    assert(!registry->export_document(AssetFormat::Text, missing, error));
    return 0;
}
