#include "TerrainGenerator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
constexpr int kReachChunks = 4096;
constexpr int kClipmapLevels = 9;
constexpr int kCellsPerAxis = 64;
constexpr int kSamplesPerAxis = kCellsPerAxis + 1;
constexpr int kBenchmarkIterations = 9;

std::uint64_t hash_word(std::uint64_t hash, std::uint32_t word) {
    constexpr std::uint64_t prime = 1099511628211ull;
    for (int byte = 0; byte < 4; ++byte) {
        hash ^= static_cast<std::uint8_t>(word >> (byte * 8));
        hash *= prime;
    }
    return hash;
}

std::uint64_t hash_sample(std::uint64_t hash, const TerrainSample& sample) {
    hash = hash_word(hash, static_cast<std::uint32_t>(sample.height));
    hash = hash_word(hash, static_cast<std::uint32_t>(sample.biome));
    hash = hash_word(hash, std::bit_cast<std::uint32_t>(sample.temperature));
    hash = hash_word(hash, std::bit_cast<std::uint32_t>(sample.moisture));
    hash = hash_word(hash, std::bit_cast<std::uint32_t>(sample.continentalness));
    hash = hash_word(hash, std::bit_cast<std::uint32_t>(sample.river));
    hash = hash_word(hash, std::bit_cast<std::uint32_t>(sample.erosion));
    hash = hash_word(hash, std::bit_cast<std::uint32_t>(sample.weirdness));
    return hash;
}

bool same_coarse_sample(const TerrainSample& lhs, const TerrainSample& rhs) {
    return lhs.height == rhs.height && lhs.biome == rhs.biome &&
           std::bit_cast<std::uint32_t>(lhs.temperature) == std::bit_cast<std::uint32_t>(rhs.temperature) &&
           std::bit_cast<std::uint32_t>(lhs.moisture) == std::bit_cast<std::uint32_t>(rhs.moisture) &&
           std::bit_cast<std::uint32_t>(lhs.continentalness) == std::bit_cast<std::uint32_t>(rhs.continentalness) &&
           std::bit_cast<std::uint32_t>(lhs.river) == std::bit_cast<std::uint32_t>(rhs.river) &&
           std::bit_cast<std::uint32_t>(lhs.erosion) == std::bit_cast<std::uint32_t>(rhs.erosion) &&
           std::bit_cast<std::uint32_t>(lhs.weirdness) == std::bit_cast<std::uint32_t>(rhs.weirdness);
}

bool valid_sample(const TerrainSample& sample) {
    const auto finite = [](float value) { return std::isfinite(value); };
    return sample.height >= 4 && sample.height <= 319 &&
           static_cast<std::uint32_t>(sample.biome) < static_cast<std::uint32_t>(BiomeType::Count) &&
           finite(sample.temperature) && finite(sample.moisture) &&
           finite(sample.continentalness) && finite(sample.river) &&
           finite(sample.erosion) && finite(sample.weirdness) &&
           sample.slope == 0.0f;
}

struct GridResult {
    std::uint64_t checksum{1469598103934665603ull};
    int minimumHeight{1000};
    int maximumHeight{-1000};
    int samples{0};
};

GridResult sample_grid() {
    GridResult result;
    int halfExtent = 256;
    int spacing = 8;
    for (int level = 0; level < kClipmapLevels; ++level) {
        const int origin = -halfExtent;
        const int innerHalfExtent = level == 0 ? 0 : halfExtent / 2;
        for (int z = 0; z < kSamplesPerAxis; ++z) {
            for (int x = 0; x < kSamplesPerAxis; ++x) {
                const int worldX = origin + x * spacing;
                const int worldZ = origin + z * spacing;
                if (innerHalfExtent > 0 &&
                    std::abs(worldX) < innerHalfExtent - spacing &&
                    std::abs(worldZ) < innerHalfExtent - spacing) continue;
                const TerrainSample sample = TerrainGenerator::sample_coarse(
                    static_cast<float>(worldX), static_cast<float>(worldZ));
                if (!valid_sample(sample)) {
                    std::cerr << "invalid coarse sample at L" << level << ':' << x << ',' << z << '\n';
                    std::exit(EXIT_FAILURE);
                }
                result.minimumHeight = std::min(result.minimumHeight, sample.height);
                result.maximumHeight = std::max(result.maximumHeight, sample.height);
                result.checksum = hash_sample(result.checksum, sample);
                ++result.samples;
            }
        }
        halfExtent *= 2;
        spacing *= 2;
    }
    return result;
}
}

int main() {
    // Warm the code/data pages before measuring. The timed result therefore
    // represents the steady-state job executed by the FAR worker.
    const GridResult warmup = sample_grid();

    const GridResult baselineResult = sample_grid();
    if (baselineResult.checksum != warmup.checksum) return EXIT_FAILURE;

    // Coarse sampling skips only slope evaluations. Every other terrain field
    // must match the authoritative sampler bit-for-bit at representative points.
    for (int z = -65536; z <= 65536; z += 32768) {
        for (int x = -65536; x <= 65536; x += 32768) {
            const float worldX = static_cast<float>(x);
            const float worldZ = static_cast<float>(z);
            const TerrainSample coarse = TerrainGenerator::sample_coarse(worldX, worldZ);
            const TerrainSample detailed = TerrainGenerator::sample(worldX, worldZ);
            if (!same_coarse_sample(coarse, detailed)) {
                std::cerr << "coarse/detail mismatch at " << worldX << ',' << worldZ << '\n';
                return EXIT_FAILURE;
            }
        }
    }

    std::array<double, kBenchmarkIterations> milliseconds{};
    volatile std::uint64_t optimizationBarrier = baselineResult.checksum;
    for (int iteration = 0; iteration < kBenchmarkIterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const GridResult result = sample_grid();
        const auto end = std::chrono::steady_clock::now();
        if (result.checksum != baselineResult.checksum || result.samples != baselineResult.samples ||
            result.minimumHeight != baselineResult.minimumHeight ||
            result.maximumHeight != baselineResult.maximumHeight) {
            std::cerr << "non-deterministic FAR grid at iteration " << iteration << '\n';
            return EXIT_FAILURE;
        }
        optimizationBarrier = optimizationBarrier ^ result.checksum;
        milliseconds[static_cast<std::size_t>(iteration)] =
            std::chrono::duration<double, std::milli>(end - start).count();
    }
    (void)optimizationBarrier;
    std::sort(milliseconds.begin(), milliseconds.end());

    std::cout << "far-clipmap-sampling samples=" << baselineResult.samples
              << " reach-chunks=" << kReachChunks
              << " levels=" << kClipmapLevels
              << " min=" << milliseconds.front() << "ms"
              << " median=" << milliseconds[milliseconds.size() / 2] << "ms"
              << " max=" << milliseconds.back() << "ms"
              << " height=" << baselineResult.minimumHeight << ".." << baselineResult.maximumHeight
              << " checksum=" << baselineResult.checksum << '\n';
    return EXIT_SUCCESS;
}
