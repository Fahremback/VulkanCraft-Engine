#include "Material.hpp"
#include "../../core/serialization/Serializer.hpp"

namespace Engine {

bool MaterialAsset::save_to_file(const std::filesystem::path& path) const {
    return Serializer::serialize_material(*this, path).success;
}

bool MaterialAsset::load_from_file(const std::filesystem::path& path) {
    return Serializer::deserialize_material(*this, path).success;
}

} // namespace Engine
