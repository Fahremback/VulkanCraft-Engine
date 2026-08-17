#include "engine/gameplay/ExplosionRuntime.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace Engine::Gameplay {

namespace {

// Material axes of one runtime block (item 10): blast resistance + heat
// flammability, mirrored from the registry JSON via the public views.
struct BlastMaterial {
    float resistance{ 0.0f };
    float flammability{ 0.0f };
};

float pressure_at(const ExplosionConfig& config, float distance) {
    if (distance >= config.blastRadius || config.blastRadius <= 0.0f)
        return 0.0f;
    const float falloff = 1.0f - distance / config.blastRadius;
    return config.maxPressure *
           std::pow(std::max(0.0f, falloff), config.pressureDecay);
}

}  // namespace

bool ExplosionConfig::load_from_json(const std::string& json,
                                     std::string& errorOut) {
    engine::sdk::JsonValue document;
    if (!engine::sdk::json_parse(json, document, errorOut)) return false;
    if (!document.is_object()) {
        errorOut = "explosion config must be a JSON object";
        return false;
    }
    // C4700 fix (findings #72): the default must read the MEMBER, not the
    // local being declared (unqualified name inside the initializer resolves
    // to the uninitialized local).
    const double blastRadius = engine::sdk::json_number(document, "blastRadius", static_cast<double>(this->blastRadius));
    const double maxPressure = engine::sdk::json_number(document, "maxPressure", static_cast<double>(this->maxPressure));
    const double pressureDecay = engine::sdk::json_number(document, "pressureDecay", static_cast<double>(this->pressureDecay));
    const double resistanceFactor = engine::sdk::json_number(document, "resistanceFactor", static_cast<double>(this->resistanceFactor));
    const double heatRadius = engine::sdk::json_number(document, "heatRadius", static_cast<double>(this->heatRadius));
    const double heatDamage = engine::sdk::json_number(document, "heatDamage", static_cast<double>(this->heatDamage));
    const double ignitionThreshold = engine::sdk::json_number(document, "ignitionThreshold", static_cast<double>(this->ignitionThreshold));
    const double impulseScale = engine::sdk::json_number(document, "impulseScale", static_cast<double>(this->impulseScale));
    const double maxCarvedCells = engine::sdk::json_number(document, "maxCarvedCells", static_cast<double>(this->maxCarvedCells));
    const double maxBurnedCells = engine::sdk::json_number(document, "maxBurnedCells", static_cast<double>(this->maxBurnedCells));
    const double maxImpulsedBodies = engine::sdk::json_number(document, "maxImpulsedBodies", static_cast<double>(this->maxImpulsedBodies));

    if (blastRadius <= 0.0 || blastRadius > 64.0) {
        errorOut = "explosion config: blastRadius must be in (0, 64]";
        return false;
    }
    if (maxPressure < 0.0) {
        errorOut = "explosion config: maxPressure cannot be negative";
        return false;
    }
    if (pressureDecay <= 0.0 || pressureDecay > 8.0) {
        errorOut = "explosion config: pressureDecay must be in (0, 8]";
        return false;
    }
    if (resistanceFactor < 0.0) {
        errorOut = "explosion config: resistanceFactor cannot be negative";
        return false;
    }
    if (heatRadius < 0.0 || heatRadius > 64.0) {
        errorOut = "explosion config: heatRadius must be in [0, 64]";
        return false;
    }
    if (heatDamage < 0.0) {
        errorOut = "explosion config: heatDamage cannot be negative";
        return false;
    }
    if (ignitionThreshold <= 0.0) {
        errorOut = "explosion config: ignitionThreshold must be positive";
        return false;
    }
    if (impulseScale < 0.0) {
        errorOut = "explosion config: impulseScale cannot be negative";
        return false;
    }
    // Budget caps (item 14): 0 = unlimited; refuse absurd ceilings so a
    // misconfigured budget cannot bypass the bound (never clamped).
    if (maxCarvedCells < 0.0 || maxCarvedCells > 1048576.0) {
        errorOut = "explosion config: maxCarvedCells must be in [0, 1048576]";
        return false;
    }
    if (maxBurnedCells < 0.0 || maxBurnedCells > 1048576.0) {
        errorOut = "explosion config: maxBurnedCells must be in [0, 1048576]";
        return false;
    }
    if (maxImpulsedBodies < 0.0 || maxImpulsedBodies > 65536.0) {
        errorOut = "explosion config: maxImpulsedBodies must be in [0, 65536]";
        return false;
    }

    this->blastRadius = static_cast<float>(blastRadius);
    this->maxPressure = static_cast<float>(maxPressure);
    this->pressureDecay = static_cast<float>(pressureDecay);
    this->resistanceFactor = static_cast<float>(resistanceFactor);
    this->heatRadius = static_cast<float>(heatRadius);
    this->heatDamage = static_cast<float>(heatDamage);
    this->ignitionThreshold = static_cast<float>(ignitionThreshold);
    this->impulseScale = static_cast<float>(impulseScale);
    this->maxCarvedCells = static_cast<std::size_t>(maxCarvedCells);
    this->maxBurnedCells = static_cast<std::size_t>(maxBurnedCells);
    this->maxImpulsedBodies = static_cast<std::size_t>(maxImpulsedBodies);
    carveTerrain = engine::sdk::json_bool(document, "carveTerrain", carveTerrain);
    return true;
}

ExplosionResult apply_explosion(engine::voxel::IVoxelWorld& world,
                                Physics::PhysicsRuntime& physics,
                                const glm::vec3& origin,
                                const ExplosionConfig& config) {
    ExplosionResult result;
    if (config.blastRadius <= 0.0f || config.maxPressure <= 0.0f) return result;

    // Material table from the world's public runtime views (registry JSON is
    // the single source). Blocks absent from the table use the registry
    // defaults (resistance 0 / flammability 0).
    std::unordered_map<std::uint32_t, BlastMaterial> materials;
    for (const engine::voxel::BlockRuntimeView& view : world.runtime_block_views()) {
        materials[view.id] = BlastMaterial{view.resistance, view.flammability};
    }
    const auto materialOf = [&](std::uint32_t id) -> BlastMaterial {
        const auto found = materials.find(id);
        return found != materials.end() ? found->second : BlastMaterial{};
    };

    const float scanRadius = std::max(config.blastRadius, config.heatRadius);
    const int scan = static_cast<int>(std::ceil(scanRadius));
    const glm::ivec3 epicenter(static_cast<int>(std::floor(origin.x)),
                               static_cast<int>(std::floor(origin.y)),
                               static_cast<int>(std::floor(origin.z)));

    // --- Terrain carve + heat: one deterministic pass over the sphere union
    // (fixed y,z,x scan order). Blast wins over heat on the same cell.
    for (int dy = -scan; dy <= scan; ++dy) {
        for (int dz = -scan; dz <= scan; ++dz) {
            for (int dx = -scan; dx <= scan; ++dx) {
                const glm::ivec3 cell = epicenter + glm::ivec3(dx, dy, dz);
                const glm::vec3 center = glm::vec3(cell) + 0.5f;
                const float distance = glm::distance(center, origin);
                if (distance > config.blastRadius && distance > config.heatRadius)
                    continue;

                const std::uint32_t block = world.get_block(cell.x, cell.y, cell.z);
                if (block == 0u) continue;  // air
        const BlastMaterial material = materialOf(block);

                bool removed = false;
                if (config.carveTerrain && distance < config.blastRadius) {
                    const float pressure = pressure_at(config, distance);
                    if (material.resistance < pressure * config.resistanceFactor) {
                        if (config.maxCarvedCells > 0 &&
                            result.blocksRemoved >= config.maxCarvedCells) {
                            ++result.carvedSkipped;  // budget cap: skip the carve
                        } else {
                            removed = true;
                            ++result.blocksRemoved;
                        }
                    }
                }
                if (!removed && distance < config.heatRadius &&
                    material.flammability > 0.0f &&
                    config.heatDamage * material.flammability >= config.ignitionThreshold) {
                    if (config.maxBurnedCells > 0 &&
                        result.blocksIgnited >= config.maxBurnedCells) {
                        ++result.burnedSkipped;  // budget cap: skip the burn
                    } else {
                        removed = true;
                        ++result.blocksIgnited;
                    }
                }
                if (removed) {
                    world.set_block(cell.x, cell.y, cell.z, 0u);
                    if (!result.affected_any()) {
                        result.affectedMin = result.affectedMax = cell;
                    } else {
                        result.affectedMin = glm::min(result.affectedMin, cell);
                        result.affectedMax = glm::max(result.affectedMax, cell);
                    }
                }
            }
        }
    }

    // --- Impulse: pressure wave on every DYNAMIC body in the blast sphere.
    // Mass (from block density, item 7) scales the response automatically —
    // the same impulse changes a heavy body's velocity far less.
    if (config.impulseScale > 0.0f) {
        const glm::vec3 extent(config.blastRadius);
        Physics::Aabb bounds{origin - extent, origin + extent};
        for (const Physics::BodyHandle handle : physics.overlap_aabb(bounds)) {
            const Physics::RigidBody* body = physics.body(handle);
            if (body == nullptr || body->motion != Physics::MotionType::Dynamic)
                continue;
            const glm::vec3 offset = body->position - origin;
            const float distance = std::sqrt(glm::dot(offset, offset));
            if (distance >= config.blastRadius) continue;
            if (config.maxImpulsedBodies > 0 &&
                result.bodiesImpulsed >= config.maxImpulsedBodies) {
                ++result.impulseSkipped;  // budget cap: skip the push
                continue;
            }
            const glm::vec3 direction =
                distance > 1.0e-6f ? offset / distance : glm::vec3(0.0f, 1.0f, 0.0f);
            physics.apply_impulse(handle, direction * (config.impulseScale * pressure_at(config, distance)));
            ++result.bodiesImpulsed;
        }
    }

    return result;
}

}  // namespace Engine::Gameplay
