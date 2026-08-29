// IMotionMatchVendor — busca de motion matching do clone vendido
// motion-matching (Daniel Holden, MIT, §8 DEPENDENCY_POLICY) atrás de
// superfície pública. Superfície self-contained: nenhum header do doador
// aparece aqui; o adapter (src/engine/sdk/MotionMatchVendor.cpp) é o ÚNICO TU
// que inclui database.h/vec.h/quat.h. Entrega o subset útil do doador — banco
// de poses com features normalizadas (pés, quadril, trajetória), busca por
// vizinho mais próximo com aceleração por bounds e custo de transição — sem
// raylib (o demo do doador é a única parte gráfica, não promovida).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// Pose da BASE DE DADOS no formato do doador: transformações LOCAIS por osso
// (offsets parent-relative + quats xyzw), como os dados mocap do demo. O
// layout de ossos segue o esqueleto humanoide do doador: 0 = root (entidade),
// 1 = quadril (hips), 4 = pé esquerdo, 8 = pé direito (mínimo 9 ossos).
struct VendorPose {
    std::vector<float> bonePositions;   // 3 floats (xyz) por osso, LOCAL
    std::vector<float> boneRotations;   // 4 floats (xyzw) por osso, LOCAL
    std::vector<float> boneVelocities;  // 3 floats (xyz) por osso, LOCAL
    std::vector<std::int32_t> boneParents;  // índice do osso pai ou -1
};

// Consulta em ESPAÇO MUNDO (estado vivo do personagem): posições/velocidades
// mundiais dos ossos + trajetória futura prevista do root (mesmos campos que
// o controlador do doador usa para os features de trajetória).
struct VendorQuery {
    std::vector<float> worldPositions;    // 3 floats por osso, mundo
    std::vector<float> worldRotations;    // 4 floats (xyzw) por osso, mundo
    std::vector<float> worldVelocities;   // 3 floats por osso, mundo
    std::vector<float> trajectoryPositions;  // 9 floats: root xyz em +20/+40/+60 frames
    std::vector<float> trajectoryRotations;  // 12 floats: 3 quats xyzw do root futuro
};

class IMotionMatchVendor {
public:
    virtual ~IMotionMatchVendor() = default;

    // Constrói o banco de poses: features (pés/quadril/trajetória) via FK do
    // doador, normalização e estrutura de aceleração. All-or-nothing: poses
    // vazias, contagem de ossos inconsistente, < 9 ossos ou pais inválidos →
    // false + errorOut, banco anterior preservado.
    virtual bool build_database(const std::vector<VendorPose>& poses,
                                std::string& errorOut) = 0;

    // Busca a pose mais similar à consulta (features normalizadas, bounds do
    // doador). currentFrame >= 0 e transitionCost > 0 ativam o custo de
    // transição do doador (continuidade entre frames). frameIndex/cost
    // (distância quadrática normalizada, 0 = idêntico). All-or-nothing:
    // consulta com layout incompatível ou banco não construído → false.
    virtual bool query(const VendorQuery& query, std::int32_t currentFrame,
                       float transitionCost, std::int32_t& frameIndex,
                       float& cost, std::string& errorOut) = 0;

    virtual std::size_t frame_count() const = 0;
    virtual std::size_t feature_count() const = 0;
};

std::unique_ptr<IMotionMatchVendor> create_motion_match_vendor();

}  // namespace animation
}  // namespace engine
