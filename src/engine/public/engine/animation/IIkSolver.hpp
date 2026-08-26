#pragma once
// IIkSolver — IK analítico determinístico (2 ossos + look-at/aim).
//
// Contrato público self-contained do §4 item 2 (unidade "IK de mãos/pés/
// olhar"). Matemática pura, SEM estado mutável (estado = "{}"): nenhuma
// dependência de core/registro — só os tipos vetoriais compartilhados de
// IAnimCore (AnimVec3/AnimQuat).
//
// Semântica:
//   solve_two_bone(origin, target, L1, L2, bend_dir):
//     resolve o triângulo (L1, L2, d=|target−origin|) no plano definido por
//     `dir = target−origin` e a projeção perpendicular de `bend_dir`.
//     d fora de [|L1−L2|, L1+L2] → clamp + `stretched=true`. Erros honestos
//     p/ comprimentos não positivos, alvo no origem e direção degenerada.
//   solve_aim(axis, target_dir, up):
//     look-at em dois passos: shortest-arc (axis → target_dir) + correção de
//     roll em torno do target p/ aproximar o `up` desejado. Anti-paralelo =
//     180° em torno de um eixo perpendicular determinístico.
//
// Escopo §4 item 2: IK (esta unidade) → retargeting → constraints
// (unidades registradas).

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>

namespace engine::animation {

struct TwoBoneResult {
    double elbow_angle = 0.0;  // ângulo no ombro/raiz (O), radianos
    double joint_angle = 0.0;  // ângulo no cotovelo (meio), radianos
    bool stretched = false;    // alvo fora do alcance → esticado (clamp)
    AnimVec3 elbow_pos;        // posição do cotovelo no plano de dobra
    AnimVec3 effector;         // posição final do effector após resolver
};

// IK analítico determinístico (sem estado).
class IIkSolver {
public:
    virtual ~IIkSolver() = default;

    // 2-bone analytic. `bend_dir` escolhe o lado da dobra (só a projeção
    // perpendicular a `dir` importa). Devolve ângulos/posições exatos do
    // triângulo resolvido; erro honesto em entradas degeneradas.
    virtual TwoBoneResult solve_two_bone(const AnimVec3& origin,
                                         const AnimVec3& target,
                                         double length_a, double length_b,
                                         const AnimVec3& bend_dir,
                                         std::string& errorOut) const = 0;

    // Look-at: quaternion que alinha `axis` com `target_dir` (shortest-arc)
    // e usa `up` p/ estabilizar o roll. Erro honesto p/ vetores nulos.
    virtual AnimQuat solve_aim(const AnimVec3& axis, const AnimVec3& target_dir,
                               const AnimVec3& up,
                               std::string& errorOut) const = 0;

    // Sem estado mutável: estado = "{}" (consistente com IRootMotion).
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IIkSolver).
std::unique_ptr<IIkSolver> create_ik_solver();

}  // namespace engine::animation
