#include "engine/gameplay/DestructionHistoryRuntime.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>

namespace Engine::Gameplay {

bool DestructionHistoryConfig::load_from_json(const std::string& json,
                                              std::string& errorOut) {
    engine::sdk::JsonValue document;
    if (!engine::sdk::json_parse(json, document, errorOut)) return false;
    if (!document.is_object()) {
        errorOut = "destruction history config must be a JSON object";
        return false;
    }
    // Member-qualified defaults: an unqualified name inside its own
    // initializer resolves to the local being declared (uninitialized).
    const double maxRegionCells = engine::sdk::json_number(
        document, "maxRegionCells", static_cast<double>(this->maxRegionCells));

    if (maxRegionCells < 1.0 || maxRegionCells > 1'000'000.0) {
        errorOut = "destruction history config: maxRegionCells must be in [1, 1000000]";
        return false;
    }

    this->maxRegionCells = static_cast<std::size_t>(maxRegionCells);
    enabled = engine::sdk::json_bool(document, "enabled", enabled);
    return true;
}

DestructionHistoryRuntime::DestructionHistoryRuntime(
    Physics::PhysicsRuntime& physics, DebrisRuntime& debris,
    DestructionHistoryConfig config)
    : physics_(physics), debris_(debris), config_(config) {}

bool DestructionHistoryRuntime::capture(engine::voxel::IVoxelWorld& world,
                                        const glm::ivec3& minimum,
                                        const glm::ivec3& maximum,
                                        DestructionSnapshot& out,
                                        std::string& errorOut) const {
    out = DestructionSnapshot{};
    if (!config_.enabled) {
        errorOut = "destruction history disabled";
        return false;
    }
    if (minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z) {
        errorOut = "destruction history: empty region (min > max)";
        return false;
    }
    const glm::ivec3 size = maximum - minimum + glm::ivec3(1);
    const std::size_t cellCount = static_cast<std::size_t>(size.x) *
                                  static_cast<std::size_t>(size.y) *
                                  static_cast<std::size_t>(size.z);
    if (cellCount > config_.maxRegionCells) {
        errorOut = "destruction history: region exceeds maxRegionCells (" +
                   std::to_string(config_.maxRegionCells) + ")";
        return false;
    }

    out.minimum = minimum;
    out.maximum = maximum;
    out.cells.reserve(cellCount);
    // Fixed (y,z,x) scan order — deterministic capture.
    for (int y = minimum.y; y <= maximum.y; ++y) {
        for (int z = minimum.z; z <= maximum.z; ++z) {
            for (int x = minimum.x; x <= maximum.x; ++x) {
                DestructionCell cell;
                cell.position = glm::ivec3(x, y, z);
                cell.blockId = world.get_block(x, y, z);
                out.cells.push_back(cell);
            }
        }
    }
    // Debris in the region, any motion state, deterministic order.
    out.debris = debris_.debris_in_box(glm::vec3(minimum), glm::vec3(maximum));
    return true;
}

DestructionRestoreResult DestructionHistoryRuntime::restore(
    engine::voxel::IVoxelWorld& world, const DestructionSnapshot& snapshot) {
    DestructionRestoreResult result;
    if (!config_.enabled || !snapshot.valid()) return result;

    // 1. Voxel cells: write every captured cell back. Only changed cells are
    //    counted (air stays air, solids come back — the region converges to
    //    the captured state regardless of the damage source).
    for (const DestructionCell& cell : snapshot.cells) {
        const std::uint32_t current = world.get_block(cell.position.x,
                                                      cell.position.y,
                                                      cell.position.z);
        if (current == cell.blockId) continue;
        world.set_block(cell.position.x, cell.position.y, cell.position.z,
                        cell.blockId);
        ++result.cellsWritten;
    }

    // 2. Debris: despawn every CURRENT debris in the region (revoxelized /
    //    moved / still-flying debris created after the capture is removed),
    //    then re-spawn the captured records through the pool (deterministic
    //    order, identity preserved).
    const glm::vec3 minF = glm::vec3(snapshot.minimum);
    const glm::vec3 maxF = glm::vec3(snapshot.maximum);
    result.debrisDespawned += debris_.despawn_in_box(minF, maxF);
    result.debrisSpawned += debris_.restore(snapshot.debris);
    return result;
}

}  // namespace Engine::Gameplay
