#include "engine/gameplay/DestructionTelemetry.hpp"

namespace Engine::Gameplay {

void DestructionTelemetry::note_spill(DestructionStage stage, std::size_t n) {
    if (n == 0) return;
    switch (stage) {
        case DestructionStage::Carve:
            step_.spilledCarved += n;
            total_.spilledCarved += n;
            break;
        case DestructionStage::Burn:
            step_.spilledBurned += n;
            total_.spilledBurned += n;
            break;
        case DestructionStage::Impulse:
            step_.spilledImpulsed += n;
            total_.spilledImpulsed += n;
            break;
        case DestructionStage::Islands:
            step_.spilledIslands += n;
            total_.spilledIslands += n;
            break;
        case DestructionStage::Spawn:
            step_.spilledDebris += n;
            total_.spilledDebris += n;
            break;
        case DestructionStage::Revoxelize:
            step_.spilledRevoxelized += n;
            total_.spilledRevoxelized += n;
            break;
        case DestructionStage::Blocks:
            step_.spilledBlocks += n;
            total_.spilledBlocks += n;
            break;
        case DestructionStage::Restore:
            step_.spilledRestored += n;
            total_.spilledRestored += n;
            break;
    }
}

bool DestructionTelemetry::exceeded(DestructionStage stage) const noexcept {
    switch (stage) {
        case DestructionStage::Carve: return step_.spilledCarved > 0;
        case DestructionStage::Burn: return step_.spilledBurned > 0;
        case DestructionStage::Impulse: return step_.spilledImpulsed > 0;
        case DestructionStage::Islands: return step_.spilledIslands > 0;
        case DestructionStage::Spawn: return step_.spilledDebris > 0;
        case DestructionStage::Revoxelize: return step_.spilledRevoxelized > 0;
        case DestructionStage::Blocks: return step_.spilledBlocks > 0;
        case DestructionStage::Restore: return step_.spilledRestored > 0;
    }
    return false;
}

}  // namespace Engine::Gameplay
