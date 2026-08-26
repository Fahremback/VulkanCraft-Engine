#pragma once
// IRootMotion — contrato público de root motion determinístico
// (agente 4 §4 item 1 — sobre o núcleo IAnimCore).
//
// Extrai o deslocamento do OSSO RAIZ de um clip entre dois tempos (o "root
// motion" clássico: o quanto o personagem se move com a animação). Puro e
// determinístico: SEM RNG, SEM relógio, SEM estado global — a mesma amostragem
// do IAnimCore + intervalo produzem o mesmo delta bit-exato. O contrato
// consome o `IAnimCore` (mesmo domínio público — o caller passa a instância);
// não há estado próprio (serialize devolve o JSON canônico vazio).
//
// Modelo:
//   - compute(core, clipId, rootBone, t0, t1): amostra o clip nos dois tempos
//     e devolve position_delta = pos(t1)−pos(t0), rotation_delta =
//     rot(t1)·rot(t0)⁻¹, distance (comprimento do delta) e
//     horizontal_distance (comprimento no plano XZ).

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>

namespace engine::animation {

struct RootMotionSample {
    AnimVec3 position_delta;       // delta do osso raiz no intervalo
    AnimQuat rotation_delta;       // rot(t1)·rot(t0)⁻¹
    double distance = 0.0;         // |position_delta|
    double horizontal_distance = 0.0;  // |(dx, dz)|
};

// Root motion (delta do osso raiz de um clip).
class IRootMotion {
public:
    virtual ~IRootMotion() = default;

    // Computa o delta de root motion no intervalo [t0, t1] (tempos finitos;
    // clamp feito pela amostragem do IAnimCore). Recusa clip/osso desconhecido.
    virtual RootMotionSample compute(IAnimCore& core, const std::string& clipId,
                                     const std::string& rootBone, double t0,
                                     double t1, std::string& errorOut) = 0;

    // Sem estado próprio — devolve o JSON canônico vazio (bit-exact) e
    // aceita-o de volta.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IRootMotion).
std::unique_ptr<IRootMotion> create_root_motion();

}  // namespace engine::animation
