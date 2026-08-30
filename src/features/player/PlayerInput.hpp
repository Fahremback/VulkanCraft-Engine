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
    // Movimento por capabilities (item 116): o EXECUTÁVEL do jogo decide aqui
    // se o agente pode nadar/escalar antes de player.update. Default true
    // preserva o comportamento para quem não passa (gates/creativo); o app
    // zera quando a capability restringida está ausente no registry.
    bool canSwim{true};   // false => dentro de fluido o jogador afunda (não nada)
    bool canClimb{true};  // false => sem auto-climb em degraus altos
    std::optional<BlockType> selectedBlock;
};
