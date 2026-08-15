#include "RadianceCacheMath.hpp"

#include <array>
#include <cassert>
#include <iostream>

int main() {
    using namespace radiance_cache_math;

    constexpr uint32_t resolution = 16;
    const glm::ivec3 initialMin = clipmap_min_cell(glm::vec3(0.0f), 0.25f, resolution);
    assert(initialMin == glm::ivec3(-8));

    // A sub-cell camera move must not churn a clipmap; crossing one probe cell
    // scrolls exactly one slab while unchanged world cells keep the same slot.
    const glm::ivec3 subCellMin = clipmap_min_cell(glm::vec3(3.99f, 0.0f, 0.0f), 0.25f, resolution);
    const glm::ivec3 scrolledMin = clipmap_min_cell(glm::vec3(4.01f, 0.0f, 0.0f), 0.25f, resolution);
    assert(subCellMin == initialMin);
    assert(scrolledMin == initialMin + glm::ivec3(1, 0, 0));
    assert(toroidal_local_index(glm::ivec3(-3, 2, 7), resolution) ==
           toroidal_local_index(glm::ivec3(-3 + 16, 2, 7), resolution));

    const std::array<ClipmapRange, 3> ranges{
        ClipmapRange{ glm::ivec3(-8), 16, 0.25f },
        ClipmapRange{ glm::ivec3(-8), 16, 1.0f / 16.0f },
        ClipmapRange{ glm::ivec3(-8), 16, 1.0f / 64.0f }
    };
    assert(select_cascade(ranges, glm::vec3(0.0f)) == 0);
    assert(select_cascade(ranges, glm::vec3(80.0f, 0.0f, 0.0f)) == 1);
    assert(select_cascade(ranges, glm::vec3(400.0f, 0.0f, 0.0f)) == 2);
    assert(select_cascade(ranges, glm::vec3(1000.0f, 0.0f, 0.0f)) == -1);

    std::cout << "Radiance cache clipmap math tests passed\n";
    return 0;
}
