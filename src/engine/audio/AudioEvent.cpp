#include "AudioEvent.hpp"
#include "../core/serialization/Serializer.hpp"

namespace Engine {

bool AudioEventAsset::save_to_file(const std::filesystem::path& path) const {
    return Serializer::serialize_audio_event(*this, path).success;
}

bool AudioEventAsset::load_from_file(const std::filesystem::path& path) {
    return Serializer::deserialize_audio_event(*this, path).success;
}

} // namespace Engine
