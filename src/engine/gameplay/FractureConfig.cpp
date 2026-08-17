#include "FractureConfig.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace Engine::Gameplay {

bool FractureConfig::load_from_json(const std::string& json, std::string& errorOut) {
    engine::sdk::JsonValue doc;
    if (!engine::sdk::json_parse(json, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "fracture config must be a JSON object";
        return false;
    }

    const int chunkSizeValue = static_cast<int>(std::lround(
        engine::sdk::json_number(doc, "chunkSize", static_cast<double>(chunkSize))));
    if (chunkSizeValue < 1 || chunkSizeValue > 16) {
        errorOut = "chunkSize must be in 1..16";
        return false;
    }
    const float health = static_cast<float>(
        engine::sdk::json_number(doc, "chunkHealth", static_cast<double>(chunkHealth)));
    if (health <= 0.0f) {
        errorOut = "chunkHealth must be > 0";
        return false;
    }
    const float resistance = static_cast<float>(
        engine::sdk::json_number(doc, "damageResistance",
                                 static_cast<double>(damageResistance)));
    if (resistance < 0.0f) {
        errorOut = "damageResistance must be >= 0";
        return false;
    }
    const float massScaleValue = static_cast<float>(
        engine::sdk::json_number(doc, "massScale", static_cast<double>(massScale)));
    if (massScaleValue <= 0.0f) {
        errorOut = "massScale must be > 0";
        return false;
    }
    const double material = engine::sdk::json_number(
        doc, "materialIndex", static_cast<double>(materialIndex));
    if (material < 0.0 || material > 255.0) {
        errorOut = "materialIndex must be in 0..255";
        return false;
    }

    enabled = engine::sdk::json_bool(doc, "enabled", enabled);
    indestructible = engine::sdk::json_bool(doc, "indestructible", indestructible);
    chunkSize = chunkSizeValue;
    chunkHealth = health;
    damageResistance = resistance;
    massScale = massScaleValue;
    materialIndex = static_cast<std::uint32_t>(material);
    return true;
}

std::vector<DestructionChunkDesc> generate_voxel_fracture_chunks(
    const std::vector<VoxelCell>& solidCells, const glm::ivec3& root,
    const std::function<float(std::uint32_t)>& densityOf,
    const FractureConfig& config) {
    std::vector<DestructionChunkDesc> chunks;
    if (!config.enabled || config.indestructible || solidCells.empty()) return chunks;

    // Bucket key ordered (y, z, x) so iteration is deterministic.
    struct Key {
        int x{ 0 }, y{ 0 }, z{ 0 };
        bool operator<(const Key& other) const {
            if (y != other.y) return y < other.y;
            if (z != other.z) return z < other.z;
            return x < other.x;
        }
    };
    std::map<Key, std::vector<VoxelCell>> buckets;
    for (const VoxelCell& cell : solidCells) {
        const int bx = static_cast<int>(std::floor(
            static_cast<float>(cell.position.x) / static_cast<float>(config.chunkSize)));
        const int by = static_cast<int>(std::floor(
            static_cast<float>(cell.position.y) / static_cast<float>(config.chunkSize)));
        const int bz = static_cast<int>(std::floor(
            static_cast<float>(cell.position.z) / static_cast<float>(config.chunkSize)));
        buckets[{ bx, by, bz }].push_back(cell);
    }

    const float size = static_cast<float>(config.chunkSize);
    const glm::vec3 rootF = glm::vec3(root);
    chunks.reserve(buckets.size());
    for (const auto& [key, cells] : buckets) {
        DestructionChunkDesc chunk;
        const glm::vec3 bucketMin(static_cast<float>(key.x) * size,
                                  static_cast<float>(key.y) * size,
                                  static_cast<float>(key.z) * size);
        chunk.localPosition = bucketMin - rootF;
        chunk.localRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        chunk.halfExtents = glm::vec3(size * 0.5f);
        // Debris mass from the block density (FALTANTES item 7 chain): the
        // bucket's first cell (scan order) decides the material.
        chunk.mass = densityOf(cells.front().blockId) *
                     static_cast<float>(cells.size()) * config.massScale;
        chunk.health = config.chunkHealth;
        chunk.damageResistance = config.damageResistance;
        chunk.materialIndex = config.materialIndex;
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

}  // namespace Engine::Gameplay
