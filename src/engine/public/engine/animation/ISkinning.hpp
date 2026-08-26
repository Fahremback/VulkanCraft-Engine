#pragma once
// ISkinning — skinning CPU determinístico (matrizes de pele + deformação).
//
// Contrato público self-contained (std only) do §4 item 49 (componente
// "skinning CPU compatível com renderer"). Auditoria: nenhum contrato de
// skinning existia no domínio — este é o primeiro (o lado GPU/rendering é de
// outro agente; aqui fica o núcleo headless testável).
//
// Semântica determinística:
//   skin_matrices(skeleton, pose): por osso (ordem de declaração da
//     skeleton) skin[i] = world[i] · bind[i]⁻¹ — world via
//     IAnimCore::local_to_world, bind via IAnimCore::bind_pose. Pose deve
//     casar com a skeleton (mesmos ossos, mesma ordem) — all-or-nothing.
//   apply_skin(skin, vertex): p' = Σ wᵢ·(skin[boneᵢ]·p) com pesos
//     NORMALIZADOS (soma ≠ 1 → divide pela soma; soma 0 → erro).
//   skin_vertices: conveniência (matrizes + deformação em um passo).
//
// Escopo §4 item 49: LOD já coberto (IAnimationLod); resta o budget de
// atualização e o lado GPU (registrados).

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

// Matriz 4x4 row-major (m[row*4+col]); skin = world · invBind.
struct SkinMatrix {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    AnimVec3 apply(const AnimVec3& p) const {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    }
};

// Vértice com até 4 influências (bone index na skeleton; -1 = sem influência).
struct SkinVertex {
    AnimVec3 position;
    int bone0 = -1;
    int bone1 = -1;
    int bone2 = -1;
    int bone3 = -1;
    double weight0 = 0.0;
    double weight1 = 0.0;
    double weight2 = 0.0;
    double weight3 = 0.0;
};

// Skinning determinístico sobre um IAnimCore (sem estado mutável).
class ISkinning {
public:
    virtual ~ISkinning() = default;

    // skin[i] = world[i] · bind[i]⁻¹, na ordem de declaração da skeleton.
    // Erros honestos p/ skeleton desconhecida e pose incompatível.
    virtual std::vector<SkinMatrix> skin_matrices(
        const std::string& skeletonId, const std::vector<BonePose>& pose,
        std::string& errorOut) const = 0;

    // p' = Σ wᵢ·(skin[boneᵢ]·p), pesos normalizados pela soma; -1 ou
    // índice fora do range = erro all-or-nothing.
    virtual AnimVec3 apply_skin(const std::vector<SkinMatrix>& skin,
                                const SkinVertex& v,
                                std::string& errorOut) const = 0;

    // Conveniência: skin_matrices + apply_skin para N vértices.
    virtual std::vector<AnimVec3> skin_vertices(
        const std::string& skeletonId, const std::vector<BonePose>& pose,
        const std::vector<SkinVertex>& vertices,
        std::string& errorOut) const = 0;

    // Sem estado mutável: estado = "{}" (consistente com IRootMotion).
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando ISkinning).
std::unique_ptr<ISkinning> create_skinning(IAnimCore& core);

}  // namespace engine::animation
