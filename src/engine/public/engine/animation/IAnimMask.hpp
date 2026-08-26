#pragma once
// IAnimMask — máscaras de ossos com peso por osso, para camadas de animação.
//
// Contrato público self-contained (std only) do §4 item 1 (unidade "masks").
// Composição direta com IAnimAdditive (#212): sample_additive → mask_deltas →
// layer_additive. Osso NÃO listado na máscara = peso 0 (não afetado).
//
// Semântica determinística de `mask_deltas(maskId, deltas)` (por delta):
//   w    = peso do osso na máscara (0 se ausente)
//   pos  = delta.pos · w
//   rot  = slerp(identidade, delta.rot, w)   (w=0 → identidade, w=1 → delta)
//   scale = lerp(1, delta.scale, w) por componente
//
// Escopo §4 item 1: CORE (#207) + state machine (#208) + root motion (#209)
// + events (#211) + additive (#212) + masks (#213) — item COMPLETO.

#include "engine/animation/IAnimAdditive.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct AnimMaskEntry {
    std::string bone;
    double weight = 1.0;  // 0..1 (0 = não afetado)
};

// Máscaras por id — pesos 0..1 por osso, serialização bit-exact.
class IAnimMask {
public:
    virtual ~IAnimMask() = default;

    // All-or-nothing: id não vazio e único; peso finito em [0,1]; osso não
    // vazio e sem duplicata na MESMA máscara.
    virtual bool add_mask(const std::string& maskId,
                          const std::vector<AnimMaskEntry>& entries,
                          std::string& errorOut) = 0;

    virtual bool has_mask(const std::string& maskId) const = 0;
    virtual std::vector<std::string> mask_ids() const = 0;

    // Peso de um osso na máscara (0.0 se o osso não está listado).
    virtual double weight(const std::string& maskId,
                          const std::string& bone) const = 0;

    // Aplica os pesos aos deltas (mesma ordem de entrada); máscara
    // desconhecida = erro all-or-nothing.
    virtual std::vector<AdditiveDelta> mask_deltas(
        const std::string& maskId, const std::vector<AdditiveDelta>& deltas,
        std::string& errorOut) const = 0;

    // Registro completo serializado / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAnimMask).
std::unique_ptr<IAnimMask> create_anim_mask();

}  // namespace engine::animation
