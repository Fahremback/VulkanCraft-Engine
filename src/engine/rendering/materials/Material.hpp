#pragma once

#include <glm/glm.hpp>
#include <string>
#include <filesystem>
#include "../../core/uuid/UUID.hpp"

namespace Engine {

struct MaterialAsset {
    UUID id;
    std::string name{ "Untitled Material" };
    glm::vec3 albedo{ 1.0f, 1.0f, 1.0f };
    float roughness{ 0.5f };
    float metallic{ 0.0f };
    glm::vec3 emissiveColor{ 0.0f, 0.0f, 0.0f };
    float emissiveIntensity{ 0.0f };
    UUID albedoMapID{ 0, 0 };
    UUID normalMapID{ 0, 0 };
    UUID roughnessMapID{ 0, 0 };
    UUID metallicMapID{ 0, 0 };

    bool save_to_file(const std::filesystem::path& path) const;
    bool load_from_file(const std::filesystem::path& path);
};

} // namespace Engine
