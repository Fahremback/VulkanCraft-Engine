#include "VisualScriptGraph.hpp"
#include "../core/serialization/Serializer.hpp"

namespace Engine {

bool VisualScriptGraph::save_to_file(const std::filesystem::path& path) const {
    return Serializer::serialize_visual_script(*this, path).success;
}

bool VisualScriptGraph::load_from_file(const std::filesystem::path& path) {
    return Serializer::deserialize_visual_script(*this, path).success;
}

} // namespace Engine
