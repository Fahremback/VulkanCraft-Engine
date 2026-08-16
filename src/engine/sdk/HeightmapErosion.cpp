// HeightmapErosion.cpp
//
// SDK adapter for engine/procgen/IHeightmapErosion.hpp (META section 18 /
// FALTANTES item 14: headless erosion with per-seed/hash/tile cache, never
// in the frame). The catalog candidate soil-machine is BLOCKED at the
// promotion gate (see DEPENDENCY_POLICY): it is an in-development OpenGL
// demo whose RNG is non-deterministic (random_device + global rand()), whose
// soil table lives in the demo app rather than a library header, and whose
// display coupling (Vertexpool/OpenGL) makes headless use a fork. This TU
// implements the same algorithm family — particle-based hydraulic erosion
// (water droplets carrying sediment down the height gradient) plus a thermal
// cascade — deterministically (seeded splitmix64, fixed order) and
// library-shaped. Self-contained, no backend.

#include "engine/procgen/IHeightmapErosion.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {

namespace {

// Deterministic splitmix64 generator: every random choice is seeded and
// consumed in a fixed order, so results are bit-identical across runs.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, 1).
    double unit() { return (next() >> 11) * (1.0 / 9007199254740992.0); }

private:
    std::uint64_t state_;
};

bool validate_spec(const ErosionSpec& spec, std::string& errorOut) {
    if (spec.iterations < 0) {
        errorOut = "erosion: iterations must be non-negative";
        return false;
    }
    if (spec.maxSteps < 1) {
        errorOut = "erosion: maxSteps must be at least 1";
        return false;
    }
    if (spec.evaporation <= 0.0f || spec.evaporation > 1.0f) {
        errorOut = "erosion: evaporation must be in (0, 1]";
        return false;
    }
    if (spec.erosionRate < 0.0f || spec.erosionRate > 1.0f ||
        spec.depositionRate < 0.0f || spec.depositionRate > 1.0f) {
        errorOut = "erosion: erosion/deposition rates must be in [0, 1]";
        return false;
    }
    if (spec.capacityScale < 0.0f) {
        errorOut = "erosion: capacityScale must be non-negative";
        return false;
    }
    if (spec.gravity <= 0.0f) {
        errorOut = "erosion: gravity must be positive";
        return false;
    }
    if (spec.minSlope < 0.0f) {
        errorOut = "erosion: minSlope must be non-negative";
        return false;
    }
    if (spec.maxSpeed <= 0.0f) {
        errorOut = "erosion: maxSpeed must be positive";
        return false;
    }
    if (spec.thermalIterations < 0) {
        errorOut = "erosion: thermalIterations must be non-negative";
        return false;
    }
    if (spec.talusAngle < 0.0f) {
        errorOut = "erosion: talusAngle must be non-negative";
        return false;
    }
    return true;
}

bool validate_heightmap(const Heightmap& map, std::string& errorOut) {
    if (map.width <= 0 || map.height <= 0) {
        errorOut = "erosion: heightmap dimensions must be positive";
        return false;
    }
    if (map.values.size() !=
        static_cast<std::size_t>(map.width) * static_cast<std::size_t>(map.height)) {
        errorOut = "erosion: heightmap values size does not match dimensions";
        return false;
    }
    return true;
}

float sample_height(const Heightmap& map, int x, int z) {
    return map.values[z * map.width + x];
}

void set_height(Heightmap& map, int x, int z, float v) {
    map.values[z * map.width + x] = v;
}

// Bilinear height at fractional (fx, fz).
float sample_bilinear(const Heightmap& map, float fx, float fz) {
    const int w = map.width;
    const int h = map.height;
    const int x0 = std::min(std::max(static_cast<int>(std::floor(fx)), 0), w - 1);
    const int z0 = std::min(std::max(static_cast<int>(std::floor(fz)), 0), h - 1);
    const int x1 = std::min(x0 + 1, w - 1);
    const int z1 = std::min(z0 + 1, h - 1);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);
    const float h00 = sample_height(map, x0, z0);
    const float h10 = sample_height(map, x1, z0);
    const float h01 = sample_height(map, x0, z1);
    const float h11 = sample_height(map, x1, z1);
    const float a = h00 + (h10 - h00) * tx;
    const float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

// Central-difference gradient at fractional (fx, fz).
void gradient_at(const Heightmap& map, float fx, float fz, float& gx,
                 float& gz) {
    const int w = map.width;
    const int h = map.height;
    const int x0 = std::min(std::max(static_cast<int>(std::floor(fx)), 0), w - 1);
    const int z0 = std::min(std::max(static_cast<int>(std::floor(fz)), 0), h - 1);
    const float hL = sample_bilinear(map, std::max(fx - 1.0f, 0.0f), fz);
    const float hR = sample_bilinear(map, std::min(fx + 1.0f, w - 1.0f), fz);
    const float hU = sample_bilinear(map, fx, std::max(fz - 1.0f, 0.0f));
    const float hD = sample_bilinear(map, fx, std::min(fz + 1.0f, h - 1.0f));
    gx = (hR - hL) * 0.5f;
    gz = (hD - hU) * 0.5f;
}

// Hydraulic pass: water droplets carry sediment down the height gradient.
// Standard conservative model — droplets accumulate velocity downhill (with
// momentum, so they can climb slightly and deposit), erode/deposit up to
// their sediment capacity, and return any remaining sediment to the map on
// death, so total material is conserved within float noise.
void hydraulic_pass(Heightmap& map, const ErosionSpec& spec, SplitMix64& rng) {
    const int w = map.width;
    const int h = map.height;
    const float maxX = static_cast<float>(w - 1);
    const float maxZ = static_cast<float>(h - 1);
    for (int i = 0; i < spec.iterations; ++i) {
        float px = static_cast<float>(rng.unit()) * maxX;
        float pz = static_cast<float>(rng.unit()) * maxZ;
        float water = 1.0f;
        float sediment = 0.0f;
        float vx = 0.0f;
        float vz = 0.0f;
        for (int step = 0; step < spec.maxSteps && water > 0.01f; ++step) {
            float gx = 0.0f;
            float gz = 0.0f;
            gradient_at(map, px, pz, gx, gz);
            const float len = std::sqrt(gx * gx + gz * gz);
            if (len < 1e-6f) {
                break;  // flat spot: droplet stops
            }
            vx += -gx / len * spec.gravity;
            vz += -gz / len * spec.gravity;
            float speed = std::sqrt(vx * vx + vz * vz);
            if (speed > spec.maxSpeed) {
                vx *= spec.maxSpeed / speed;
                vz *= spec.maxSpeed / speed;
            }
            const float nx = px + vx;
            const float nz = pz + vz;
            if (nx < 0.0f || nx > maxX || nz < 0.0f || nz > maxZ) {
                break;  // out of bounds (sediment returned below)
            }
            const float hOld = sample_bilinear(map, px, pz);
            const float hNew = sample_bilinear(map, nx, nz);
            const float climb = hNew - hOld;  // > 0 going uphill
            const float capacity =
                std::max(-climb, spec.minSlope) * water * spec.capacityScale;
            const int cx = std::min(static_cast<int>(nx), w - 1);
            const int cz = std::min(static_cast<int>(nz), h - 1);
            if (sediment > capacity) {
                // Deposit while climbing (or on the flat).
                const float amount =
                    std::min(climb, sediment - capacity) * spec.depositionRate;
                if (amount > 0.0f) {
                    set_height(map, cx, cz,
                               sample_height(map, cx, cz) + amount);
                    sediment -= amount;
                }
            } else {
                // Erode the cell we land on, up to capacity.
                const float amount =
                    std::min(-climb, capacity - sediment) * spec.erosionRate;
                if (amount > 0.0f) {
                    set_height(map, cx, cz,
                               sample_height(map, cx, cz) - amount);
                    sediment += amount;
                }
            }
            px = nx;
            pz = nz;
            water *= (1.0f - spec.evaporation);
        }
        // Return remaining sediment to the map (at the last valid position,
        // also on out-of-bounds) so material is conserved.
        if (sediment > 0.0f) {
            const int cx = std::min(static_cast<int>(px), w - 1);
            const int cz = std::min(static_cast<int>(pz), h - 1);
            set_height(map, cx, cz,
                       sample_height(map, cx, cz) + sediment);
        }
    }
}

// Thermal pass: material slides off slopes steeper than the talus angle
// (fixed row-major order, so deterministic by construction).
void thermal_pass(Heightmap& map, int iterations, float talusAngle) {
    const int w = map.width;
    const int h = map.height;
    for (int pass = 0; pass < iterations; ++pass) {
        for (int z = 0; z < h; ++z) {
            for (int x = 0; x < w; ++x) {
                const float h0 = sample_height(map, x, z);
                float hLow = h0;
                int lowX = x;
                int lowZ = z;
                const int nx[4] = { x - 1, x + 1, x, x };
                const int nz[4] = { z, z, z - 1, z + 1 };
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || nx[k] >= w || nz[k] < 0 || nz[k] >= h) {
                        continue;
                    }
                    const float hn = sample_height(map, nx[k], nz[k]);
                    if (hn < hLow) {
                        hLow = hn;
                        lowX = nx[k];
                        lowZ = nz[k];
                    }
                }
                const float diff = h0 - hLow;
                if (diff > talusAngle) {
                    const float amount = (diff - talusAngle) * 0.5f;
                    set_height(map, x, z, h0 - amount);
                    set_height(map, lowX, lowZ,
                               sample_height(map, lowX, lowZ) + amount);
                }
            }
        }
    }
}

}  // namespace

class HeightmapErosion final : public IHeightmapErosion {
public:
    bool erode(const Heightmap& in, const ErosionSpec& spec, Heightmap& out,
               std::string& errorOut) override {
        if (!validate_spec(spec, errorOut) ||
            !validate_heightmap(in, errorOut)) {
            return false;
        }
        out = in;
        if (spec.iterations > 0) {
            SplitMix64 rng(spec.seed);
            hydraulic_pass(out, spec, rng);
        }
        if (spec.thermalIterations > 0) {
            thermal_pass(out, spec.thermalIterations, spec.talusAngle);
        }
        return true;
    }

    bool validate(const ErosionSpec& spec, std::string& errorOut) const override {
        return validate_spec(spec, errorOut);
    }

    bool serialize_spec(const ErosionSpec& spec,
                        std::string& out) const override {
        std::ostringstream ss;
        ss.precision(9);
        ss << "{\"version\":1,\"seed\":" << spec.seed
           << ",\"iterations\":" << spec.iterations
           << ",\"maxSteps\":" << spec.maxSteps
           << ",\"evaporation\":" << spec.evaporation
           << ",\"erosionRate\":" << spec.erosionRate
           << ",\"depositionRate\":" << spec.depositionRate
           << ",\"capacityScale\":" << spec.capacityScale
           << ",\"gravity\":" << spec.gravity
           << ",\"minSlope\":" << spec.minSlope
           << ",\"maxSpeed\":" << spec.maxSpeed
           << ",\"thermalIterations\":" << spec.thermalIterations
           << ",\"talusAngle\":" << spec.talusAngle << '}';
        out = ss.str();
        return true;
    }

    bool deserialize_spec(const std::string& json, ErosionSpec& out,
                          std::string& errorOut) const override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "erosion: malformed spec - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "erosion: spec must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "erosion: unsupported spec version";
            return false;
        }
        ErosionSpec parsed;
        parsed.seed = static_cast<std::uint64_t>(
            sdk::json_number(document, "seed", 1.0));
        parsed.iterations =
            static_cast<int>(sdk::json_number(document, "iterations", 0.0));
        parsed.maxSteps =
            static_cast<int>(sdk::json_number(document, "maxSteps", 1.0));
        parsed.evaporation = static_cast<float>(
            sdk::json_number(document, "evaporation", 0.02));
        parsed.erosionRate = static_cast<float>(
            sdk::json_number(document, "erosionRate", 0.08));
        parsed.depositionRate = static_cast<float>(
            sdk::json_number(document, "depositionRate", 0.08));
        parsed.capacityScale = static_cast<float>(
            sdk::json_number(document, "capacityScale", 0.05));
        parsed.gravity =
            static_cast<float>(sdk::json_number(document, "gravity", 4.0));
        parsed.minSlope =
            static_cast<float>(sdk::json_number(document, "minSlope", 0.01));
        parsed.maxSpeed =
            static_cast<float>(sdk::json_number(document, "maxSpeed", 6.0));
        parsed.thermalIterations = static_cast<int>(
            sdk::json_number(document, "thermalIterations", 0.0));
        parsed.talusAngle = static_cast<float>(
            sdk::json_number(document, "talusAngle", 0.08));
        // All-or-nothing: only commit a valid spec.
        if (!validate_spec(parsed, errorOut)) {
            return false;
        }
        out = parsed;
        return true;
    }
};

class TileErosionCache final : public ITileErosionCache {
public:
    TileErosionCache() : erosion_(create_heightmap_erosion()) {}

    bool erode_tile(const ErosionSpec& spec, int tileX, int tileY,
                    const Heightmap& tile, Heightmap& out,
                    std::string& errorOut) override {
        std::string specJson;
        if (!erosion_->serialize_spec(spec, specJson)) {
            errorOut = "erosion cache: cannot serialize spec";
            return false;
        }
        const std::string key = make_key(spec.seed, specJson, tileX, tileY);
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            out = found->second;
            return true;
        }
        Heightmap eroded;
        if (!erosion_->erode(tile, spec, eroded, errorOut)) {
            return false;
        }
        entries_.emplace(key, eroded);
        out = std::move(eroded);
        return true;
    }

    void clear() override { entries_.clear(); }

    std::size_t size() const override { return entries_.size(); }

private:
    static std::string make_key(std::uint64_t seed, const std::string& specJson,
                                int tileX, int tileY) {
        std::ostringstream ss;
        ss << "v1:" << seed << ':' << specJson << ':' << tileX << ':' << tileY;
        return ss.str();
    }

    std::shared_ptr<IHeightmapErosion> erosion_;
    std::unordered_map<std::string, Heightmap> entries_;
};

}  // namespace procgen
}  // namespace engine

namespace engine {
namespace procgen {

std::shared_ptr<IHeightmapErosion> create_heightmap_erosion() {
    return std::make_shared<HeightmapErosion>();
}

std::shared_ptr<ITileErosionCache> create_tile_erosion_cache() {
    return std::make_shared<TileErosionCache>();
}

}  // namespace procgen
}  // namespace engine
