#pragma once
// IAnimAdditive — animação aditiva: delta de pose em relação a uma referência,
// aplicável sobre uma pose base.
//
// Contrato público self-contained (std only) do §4 item 1 (unidade "additive").
// O adapter recebe `IAnimCore&` na fábrica e amostra clips por ele — sem
// acoplar o contrato ao núcleo além da fábrica.
//
// Semântica determinística:
//   sample_additive(clip, t, ref) = pose(clip,t) ⊖ pose(clip,ref) com
//     pos   = pos(t) − pos(ref)
//     rot   = rot(t) · rot(ref)⁻¹   (delta no espaço local do osso)
//     scale = scale(t) / scale(ref) (componente a componente)
//   layer_additive(base, deltas): pos = base.pos + delta.pos;
//     rot = base.rot · delta.rot; scale = base.scale · delta.scale.
// Ossos sem trilha no clip = bind local → delta identidade (não altera base).
//
// Escopo §4 item 1: CORE (#207) + state machine (#208) + root motion (#209)
// + events (#211) + additive (#212); resta masks (unidade registrada).

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct AdditiveDelta {
    std::string bone;
    AnimTransform local;  // delta: posição somada, rotação composta,
                          // escala multiplicada
};

// Aditivo determinístico sobre um IAnimCore (sem estado mutável).
class IAnimAdditive {
public:
    virtual ~IAnimAdditive() = default;

    // Delta do clip em `t` relativo à pose em `refTime` (ambos clampados em
    // [0, duration]). Devolve um delta por osso da skeleton (ordem de
    // declaração); erro honesto p/ clip desconhecido.
    virtual std::vector<AdditiveDelta> sample_additive(
        const std::string& clipId, double t, double refTime,
        std::string& errorOut) const = 0;

    // Aplica os deltas sobre uma pose base (mesma skeleton). Osso do delta
    // ausente na base = erro all-or-nothing (nada aplicado). Devolve a pose
    // base com os deltas aplicados; ossos sem delta permanecem intactos.
    virtual std::vector<BonePose> layer_additive(
        const std::vector<BonePose>& base,
        const std::vector<AdditiveDelta>& deltas,
        std::string& errorOut) const = 0;

    // Sem estado mutável: estado = "{}" (consistente com IRootMotion).
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAnimAdditive).
std::unique_ptr<IAnimAdditive> create_anim_additive(IAnimCore& core);

}  // namespace engine::animation
