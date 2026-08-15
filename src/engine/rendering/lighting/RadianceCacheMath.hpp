#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <span>

namespace radiance_cache_math {

struct ClipmapRange {
    glm::ivec3 minCell{ 0 };
    int resolution{ 0 };
    float inverseSpacing{ 1.0f };
};

inline glm::ivec3 clipmap_min_cell(const glm::vec3& cameraPosition,
                                   float inverseSpacing,
                                   uint32_t resolution) {
    const glm::ivec3 center = glm::ivec3(glm::floor(cameraPosition * inverseSpacing));
    return center - glm::ivec3(static_cast<int>(resolution / 2u));
}

inline int positive_mod(int value, int divisor) {
    const int result = value % divisor;
    return result < 0 ? result + divisor : result;
}

inline uint32_t toroidal_local_index(const glm::ivec3& worldCell, uint32_t resolution) {
    const int divisor = static_cast<int>(resolution);
    const uint32_t x = static_cast<uint32_t>(positive_mod(worldCell.x, divisor));
    const uint32_t y = static_cast<uint32_t>(positive_mod(worldCell.y, divisor));
    const uint32_t z = static_cast<uint32_t>(positive_mod(worldCell.z, divisor));
    return (z * resolution + y) * resolution + x;
}

inline bool contains(const ClipmapRange& range, const glm::vec3& worldPosition, int guardCells = 1) {
    const glm::ivec3 cell = glm::ivec3(glm::floor(worldPosition * range.inverseSpacing));
    const glm::ivec3 local = cell - range.minCell;
    const int upper = range.resolution - guardCells;
    return local.x >= guardCells && local.y >= guardCells && local.z >= guardCells &&
           local.x < upper && local.y < upper && local.z < upper;
}

inline int select_cascade(std::span<const ClipmapRange> ranges, const glm::vec3& worldPosition) {
    for (size_t index = 0; index < ranges.size(); ++index) {
        if (contains(ranges[index], worldPosition)) return static_cast<int>(index);
    }
    return -1;
}

} // namespace radiance_cache_math
