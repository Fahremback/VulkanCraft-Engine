#include "engine/gameplay/DestructionBudget.hpp"

#include "engine/sdk/RegistryJson.hpp"

namespace Engine::Gameplay {

bool DestructionBudget::load_from_json(const std::string& json,
                                       std::string& errorOut) {
    engine::sdk::JsonValue document;
    if (!engine::sdk::json_parse(json, document, errorOut)) return false;
    if (!document.is_object()) {
        errorOut = "destruction budget must be a JSON object";
        return false;
    }

    // C4700 guard (findings #72): read the MEMBER defaults with this->.
    const double maxCarvedCells =
        engine::sdk::json_number(document, "maxCarvedCells",
                                 static_cast<double>(this->maxCarvedCells));
    const double maxBurnedCells =
        engine::sdk::json_number(document, "maxBurnedCells",
                                 static_cast<double>(this->maxBurnedCells));
    const double maxImpulsedBodies =
        engine::sdk::json_number(document, "maxImpulsedBodies",
                                 static_cast<double>(this->maxImpulsedBodies));
    const double maxDetachedIslands =
        engine::sdk::json_number(document, "maxDetachedIslands",
                                 static_cast<double>(this->maxDetachedIslands));
    const double maxSpawnedDebris =
        engine::sdk::json_number(document, "maxSpawnedDebris",
                                 static_cast<double>(this->maxSpawnedDebris));
    const double maxRevoxelizedDebris =
        engine::sdk::json_number(document, "maxRevoxelizedDebris",
                                 static_cast<double>(this->maxRevoxelizedDebris));
    const double maxRevoxelizedBlocks =
        engine::sdk::json_number(document, "maxRevoxelizedBlocks",
                                 static_cast<double>(this->maxRevoxelizedBlocks));
    const double maxRestoredCells =
        engine::sdk::json_number(document, "maxRestoredCells",
                                 static_cast<double>(this->maxRestoredCells));

    // All-or-nothing validation, mirroring the leaf ranges. 0 = unlimited for
    // the explosion/connectivity/debris gates; the revoxelize/history caps
    // keep the leaf's [1, N] (a 0 there would bypass the bound).
    if (maxCarvedCells < 0.0 || maxCarvedCells > 1048576.0) {
        errorOut = "destruction budget: maxCarvedCells must be in [0, 1048576]";
        return false;
    }
    if (maxBurnedCells < 0.0 || maxBurnedCells > 1048576.0) {
        errorOut = "destruction budget: maxBurnedCells must be in [0, 1048576]";
        return false;
    }
    if (maxImpulsedBodies < 0.0 || maxImpulsedBodies > 65536.0) {
        errorOut = "destruction budget: maxImpulsedBodies must be in [0, 65536]";
        return false;
    }
    if (maxDetachedIslands < 0.0 || maxDetachedIslands > 4096.0) {
        errorOut = "destruction budget: maxDetachedIslands must be in [0, 4096]";
        return false;
    }
    if (maxSpawnedDebris < 0.0 || maxSpawnedDebris > 4096.0) {
        errorOut = "destruction budget: maxSpawnedDebris must be in [0, 4096]";
        return false;
    }
    if (maxRevoxelizedDebris < 1.0 || maxRevoxelizedDebris > 1024.0) {
        errorOut = "destruction budget: maxRevoxelizedDebris must be in [1, 1024]";
        return false;
    }
    if (maxRevoxelizedBlocks < 1.0 || maxRevoxelizedBlocks > 4096.0) {
        errorOut = "destruction budget: maxRevoxelizedBlocks must be in [1, 4096]";
        return false;
    }
    if (maxRestoredCells < 1.0 || maxRestoredCells > 1000000.0) {
        errorOut = "destruction budget: maxRestoredCells must be in [1, 1000000]";
        return false;
    }

    this->maxCarvedCells = static_cast<std::size_t>(maxCarvedCells);
    this->maxBurnedCells = static_cast<std::size_t>(maxBurnedCells);
    this->maxImpulsedBodies = static_cast<std::size_t>(maxImpulsedBodies);
    this->maxDetachedIslands = static_cast<std::size_t>(maxDetachedIslands);
    this->maxSpawnedDebris = static_cast<std::size_t>(maxSpawnedDebris);
    this->maxRevoxelizedDebris = static_cast<std::size_t>(maxRevoxelizedDebris);
    this->maxRevoxelizedBlocks = static_cast<std::size_t>(maxRevoxelizedBlocks);
    this->maxRestoredCells = static_cast<std::size_t>(maxRestoredCells);
    enabled = engine::sdk::json_bool(document, "enabled", enabled);
    return true;
}

void DestructionBudget::apply_to(ExplosionConfig& config) const {
    if (!enabled) return;
    if (maxCarvedCells > 0) config.maxCarvedCells = maxCarvedCells;
    if (maxBurnedCells > 0) config.maxBurnedCells = maxBurnedCells;
    if (maxImpulsedBodies > 0) config.maxImpulsedBodies = maxImpulsedBodies;
}

void DestructionBudget::apply_to(RevoxelizePolicy& policy) const {
    if (!enabled) return;
    policy.maxDebrisPerPass = static_cast<int>(maxRevoxelizedDebris);
    policy.maxBlocksPerDebris = static_cast<int>(maxRevoxelizedBlocks);
}

void DestructionBudget::apply_to(DestructionHistoryConfig& config) const {
    if (!enabled) return;
    config.maxRegionCells = maxRestoredCells;
}

}  // namespace Engine::Gameplay
