#pragma once

#include "Voxel.hpp"

#include <optional>

struct PlayerInput {
    bool forward{false};
    bool backward{false};
    bool left{false};
    bool right{false};
    bool jump{false};
    bool descend{false};
    std::optional<BlockType> selectedBlock;
};
