// ErosionGateTests — gate do contrato público de erosão de heightmap
// (IHeightmapErosion / ITileErosionCache). Fecha o item G.soil-machine: a
// contraparte headless/determinística do algoritmo de erosão do catálogo
// (hidráulica por partículas + cascata térmica), implementada do zero no
// SDK, sem TinyEngine/OpenGL.
//
// Prova: determinismo bit-exact, conservação de material, validação
// all-or-nothing, round-trip de spec e o cache de tile por (seed, spec,
// tileX, tileY).

#include "engine/procgen/IHeightmapErosion.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

// Heightmap 33x33 com um pico central e declives suaves.
engine::procgen::Heightmap make_heightmap() {
    engine::procgen::Heightmap map;
    map.width = 33;
    map.height = 33;
    map.values.assign(static_cast<std::size_t>(map.width) * map.height, 0.4f);
    for (int z = 0; z < map.height; ++z) {
        for (int x = 0; x < map.width; ++x) {
            const float dx = static_cast<float>(x - 16) / 16.0f;
            const float dz = static_cast<float>(z - 16) / 16.0f;
            map.values[static_cast<std::size_t>(z) * map.width + x] =
                0.4f + 0.5f * std::exp(-(dx * dx + dz * dz) * 3.0f);
        }
    }
    return map;
}

double sum_of(const engine::procgen::Heightmap& map) {
    double sum = 0.0;
    for (const float v : map.values) sum += v;
    return sum;
}

using engine::procgen::create_heightmap_erosion;
using engine::procgen::create_tile_erosion_cache;
using engine::procgen::ErosionSpec;
using engine::procgen::Heightmap;

void test_determinism_and_shape() {
    auto erosion = create_heightmap_erosion();
    const Heightmap in = make_heightmap();
    ErosionSpec spec;
    spec.seed = 42;
    spec.iterations = 5000;

    Heightmap a;
    Heightmap b;
    std::string error;
    check(erosion->erode(in, spec, a, error) && error.empty(),
          "erode ok (a)");
    check(erosion->erode(in, spec, b, error) && error.empty(),
          "erode ok (b)");
    check(a.width == in.width && a.height == in.height,
          "output dimensions match");
    check(a.values == b.values, "deterministic: same (heightmap, spec) "
                                "produce bit-identical output");
    // O material é conservado (redistribuição hidráulica + cascata térmica)
    // dentro de ruído de ponto flutuante.
    const double delta = std::fabs(sum_of(a) - sum_of(in));
    check(delta < 1e-2 * static_cast<double>(in.values.size()),
          "material conserved within float noise");
    // O pico foi suavizado (o centro perde altura média).
    const int center = 16 * a.width + 16;
    double before = 0.0;
    double after = 0.0;
    for (int z = 13; z <= 19; ++z)
        for (int x = 13; x <= 19; ++x) {
            before += in.values[z * in.width + x];
            after += a.values[z * a.width + x];
        }
    check(after < before, "peak eroded (center region loses height)");
}

void test_validation_and_roundtrip() {
    auto erosion = create_heightmap_erosion();
    ErosionSpec spec;
    std::string error;

    ErosionSpec bad = spec;
    bad.maxSteps = 0;
    check(!erosion->validate(bad, error) && !error.empty(),
          "maxSteps 0 refused");

    bad = spec;
    bad.evaporation = 0.0f;
    check(!erosion->validate(bad, error) && !error.empty(),
          "evaporation 0 refused");

    bad = spec;
    bad.gravity = 0.0f;
    check(!erosion->validate(bad, error) && !error.empty(),
          "gravity 0 refused");

    // Round-trip de spec via JSON (version 1).
    ErosionSpec original;
    original.seed = 7;
    original.iterations = 1234;
    original.thermalIterations = 3;
    std::string json;
    check(erosion->serialize_spec(original, json) && !json.empty(),
          "serialize ok");
    ErosionSpec loaded;
    check(erosion->deserialize_spec(json, loaded, error) && error.empty(),
          "deserialize ok");
    check(loaded.seed == original.seed &&
              loaded.iterations == original.iterations &&
              loaded.thermalIterations == original.thermalIterations,
          "round-trip fields preserved");
    check(!erosion->deserialize_spec(
              "{\"version\":2,\"seed\":1,\"iterations\":10}", loaded, error),
          "unsupported version refused");
}

void test_tile_cache() {
    auto cache = create_tile_erosion_cache();
    check(cache->size() == 0, "cache starts empty");
    const Heightmap tile = make_heightmap();
    ErosionSpec spec;
    spec.seed = 99;

    Heightmap a;
    Heightmap b;
    std::string error;
    check(cache->erode_tile(spec, 3, -2, tile, a, error) && error.empty(),
          "erode_tile ok (miss)");
    check(cache->size() == 1, "cache stores the tile");
    check(cache->erode_tile(spec, 3, -2, tile, b, error) && error.empty(),
          "erode_tile ok (hit)");
    check(cache->size() == 1, "cache hit does not grow the cache");
    check(a.values == b.values, "cache hit returns the same result");

    Heightmap c;
    check(cache->erode_tile(spec, 4, -2, tile, c, error) && error.empty(),
          "erode_tile different tile");
    check(cache->size() == 2, "different tile is a separate entry");

    cache->clear();
    check(cache->size() == 0, "clear drops all entries");
}

}  // namespace

int main() {
    test_determinism_and_shape();
    test_validation_and_roundtrip();
    test_tile_cache();

    if (g_failures == 0) {
        std::cout << "erosion_gate_tests: all checks passed\n";
        return 0;
    }
    std::cout << "erosion_gate_tests: " << g_failures << " failure(s)\n";
    return 1;
}
