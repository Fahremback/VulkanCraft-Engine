#pragma once

#include <string>
#include <filesystem>
#include "../core/uuid/UUID.hpp"

namespace Engine {

struct AudioEventAsset {
    UUID id;
    std::string name{ "Untitled Audio Event" };
    std::string clipPath;
    float volume{ 1.0f };
    float minPitch{ 0.9f };
    float maxPitch{ 1.1f };
    float maxDistance{ 100.0f };
    bool is3D{ true };
    bool isLooping{ false };

    bool save_to_file(const std::filesystem::path& path) const;
    bool load_from_file(const std::filesystem::path& path);
};

} // namespace Engine
