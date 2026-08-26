#pragma once
// IRetargeting — retargeting de animação entre skeletons (fonte → alvo).
//
// Contrato público self-contained (std only) do §4 item 2 (unidade
// "retargeting"). O adapter recebe `IAnimCore&` na fábrica: valida as
// skeletons/clips no registro e usa `sample_clip`/`bind_pose` do núcleo.
//
// Semântica determinística:
//   add_retarget(id, srcSkel, dstSkel, mappings): all-or-nothing — ambas as
//     skeletons existem no core; source_bone ∈ srcSkel; target_bone ∈ dstSkel;
//     scale finito > 0; target_bone sem duplicata no MESMO retarget.
//   retarget_pose(id, clip, t): amostra o clip (registrado NA skeleton fonte)
//     em t e produz a pose NA ORDEM da skeleton alvo:
//       osso mapeado   → local da fonte com posição × mapping.scale
//                        (rotação e escala do transform copiadas)
//       osso não mapeado → bind local da skeleton alvo (via IAnimCore)
//
// Escopo §4 item 2: IK (#215) + retargeting (esta unidade); restam
// constraints e adaptação ao terreno (unidades registradas).

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct RetargetMapping {
    std::string source_bone;
    std::string target_bone;
    double scale = 1.0;  // fator de posição (normalização de proporção)
};

// Retargeting determinístico sobre um IAnimCore.
class IRetargeting {
public:
    virtual ~IRetargeting() = default;

    // All-or-nothing: valida skeletons/ossos/escala/duplicatas no registro.
    virtual bool add_retarget(const std::string& retargetId,
                              const std::string& sourceSkeleton,
                              const std::string& targetSkeleton,
                              const std::vector<RetargetMapping>& mappings,
                              std::string& errorOut) = 0;

    virtual bool has_retarget(const std::string& retargetId) const = 0;
    virtual std::vector<std::string> retarget_ids() const = 0;

    // Reamostra `clip` (da skeleton fonte) em t e mapeia para a skeleton
    // alvo (ordem de declaração da alvo; ossos sem mapeamento = bind).
    // Erros honestos p/ retarget/clip desconhecidos e clip de outra skeleton.
    virtual std::vector<BonePose> retarget_pose(const std::string& retargetId,
                                                const std::string& clipId,
                                                double t,
                                                std::string& errorOut) const = 0;

    // Registro completo serializado / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IRetargeting).
std::unique_ptr<IRetargeting> create_retargeting(IAnimCore& core);

}  // namespace engine::animation
