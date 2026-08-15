#include "TerrainGenerator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>

int main() {
    // 4096 logical chunks are a 64x64 square at two FAR cells per chunk:
    // 129x129 samples. This benchmark is informational (never timing-gated),
    // but also protects the cheap one-router-evaluation API from regressions.
    const auto farStart = std::chrono::steady_clock::now();
    std::uint64_t farChecksum = 0;
    for (int z = 0; z <= 128; ++z) {
        for (int x = 0; x <= 128; ++x) {
            const TerrainSample coarse = TerrainGenerator::sample_coarse(
                static_cast<float>((x - 64) * 8), static_cast<float>((z - 64) * 8));
            if (coarse.height < 4 || coarse.height > 319) return EXIT_FAILURE;
            farChecksum += static_cast<std::uint64_t>(coarse.height);
        }
    }
    const auto farMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - farStart).count();

    constexpr std::size_t biomeCount = static_cast<std::size_t>(BiomeType::Count);
    std::array<int, biomeCount> biomeCounts{};
    int minimumHeight = 1000;
    int maximumHeight = -1000;

    for (int z = -8192; z <= 8192; z += 32) {
        for (int x = -8192; x <= 8192; x += 32) {
            const TerrainSample terrain = TerrainGenerator::sample(static_cast<float>(x), static_cast<float>(z));
            const TerrainSample repeated = TerrainGenerator::sample(static_cast<float>(x), static_cast<float>(z));
            if (terrain.height != repeated.height || terrain.biome != repeated.biome) return EXIT_FAILURE;
            if (terrain.height < 4 || terrain.height > 319) return EXIT_FAILURE;
            minimumHeight = std::min(minimumHeight, terrain.height);
            maximumHeight = std::max(maximumHeight, terrain.height);
            ++biomeCounts.at(static_cast<std::size_t>(terrain.biome));
        }
    }

    int caveSamples = 0;
    int carvedSamples = 0;
    int upperRockSamples = 0;
    int upperRockCarvedSamples = 0;
    int longestVerticalVoid = 0;
    int longestVoidX = 0;
    int longestVoidZ = 0;
    int longestVoidTop = 0;
    for (int z = -256; z <= 256; z += 8) {
        for (int x = -256; x <= 256; x += 8) {
            for (int y = 4; y <= 112; y += 4) {
                ++caveSamples;
                if (TerrainGenerator::cave_density(static_cast<float>(x), static_cast<float>(y),
                                                     static_cast<float>(z)) > 0.0f) ++carvedSamples;
            }
        }
    }

    for (int z = -512; z <= 512; z += 8) {
        for (int x = -512; x <= 512; x += 8) {
            const TerrainSample terrain = TerrainGenerator::sample(static_cast<float>(x), static_cast<float>(z));
            int verticalVoid = 0;
            for (int y = 4; y <= terrain.height - 5; ++y) {
                const int depth = terrain.height - y;
                const bool carved = depth > 12 &&
                    TerrainGenerator::cave_density(static_cast<float>(x), static_cast<float>(y),
                                                   static_cast<float>(z)) > 0.0f;
                verticalVoid = carved ? verticalVoid + 1 : 0;
                if (verticalVoid > longestVerticalVoid) {
                    longestVerticalVoid = verticalVoid;
                    longestVoidX = x;
                    longestVoidZ = z;
                    longestVoidTop = y;
                }
                if (depth <= 40) {
                    ++upperRockSamples;
                    if (carved) ++upperRockCarvedSamples;
                }
            }
        }
    }

    const float caveRatio = static_cast<float>(carvedSamples) / static_cast<float>(caveSamples);
    const float upperRockCaveRatio = static_cast<float>(upperRockCarvedSamples) /
                                     static_cast<float>(upperRockSamples);
    const int representedBiomes = static_cast<int>(std::count_if(
        biomeCounts.begin(), biomeCounts.end(), [](int count) { return count > 0; }));
    std::cout << "far-4096=" << farMs << "ms checksum=" << farChecksum
              << " height=" << minimumHeight << ".." << maximumHeight
              << " biomes=" << representedBiomes << '/' << biomeCount
              << " caves=" << caveRatio
              << " upper-rock-caves=" << upperRockCaveRatio
              << " longest-vertical-void=" << longestVerticalVoid
              << "@(" << longestVoidX << ',' << longestVoidTop << ',' << longestVoidZ << ")\n";

    if (minimumHeight > TerrainGenerator::SeaLevel - 20) return EXIT_FAILURE;
    if (maximumHeight < 280) return EXIT_FAILURE;
    if (representedBiomes < 14) return EXIT_FAILURE;
    // Regressão visual: a antiga união de isosuperfícies removia 37,6% da
    // rocha e permitia vazios verticais de 166 blocos, deixando continentes
    // suspensos. Cavernas continuam presentes, mas não podem ocar o terreno.
    if (caveRatio < 0.005f || caveRatio > 0.12f) return EXIT_FAILURE;
    if (upperRockCaveRatio > 0.08f) return EXIT_FAILURE;
    if (longestVerticalVoid > 64) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
