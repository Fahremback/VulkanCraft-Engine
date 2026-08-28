// IMultibodyDynamics.hpp
//
// PUBLIC seam para dinâmica de corpos rígidos articulados (cadeias
// cinemáticas). É a contraparte headless/determinística do catálogo
// project-chrono (multibody dynamics) — implementada do zero no SDK, sem
// dependência externa, com dinâmica em coordenadas generalizadas:
//   - estado q = ângulos das juntas (revolute) / deslocamentos (prismatic);
//   - cinemática direta p_i(q) do COM de cada link a partir do pai;
//   - Jacobianos J_i = dp_i/dq (pilhas de 3 componentes por junta);
//   - matriz de massa M = Σ m_i · J_iᵀ J_i (+ inércia rotacional);
//   - dinâmica: q̈ = M⁻¹ (τ − g(q)), g = forças generalizadas de gravidade;
//   - integração semi-implícita de Euler + limites de junta.
//
// Escopo: o ALGORITMO multibody (braços robóticos, escavadeiras, guindastes,
// criaturas articuladas, suspensões) — headless, determinístico, consumível
// por gameplay, animação procedural e física de máquinas. NÃO substitui o
// Jolt (autoridade de rigid bodies); atua em coordenadas de junta.
//
// Determinismo: ordem fixa de montagem e de resolução linear (eliminação de
// Gauss com pivotamento parcial, sem dependência de threading).

#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace physics {

enum class JointKind : std::uint8_t {
    Revolute,  // rotação livre em torno de jointAxis
    Prismatic  // deslizamento livre ao longo de jointAxis
};

struct MultibodyLinkDesc {
    JointKind joint{ JointKind::Revolute };
    glm::vec3 jointAxis{ 0.0f, 0.0f, 1.0f };  // (normalizado; revolute/prismatic)
    glm::vec3 offset{ 0.0f, 0.0f, 0.0f };     // COM do link no frame do pai
    float mass{ 1.0f };                        // > 0
    glm::vec3 inertia{ 1.0f, 1.0f, 1.0f };     // inércia principal (> 0)
    float jointMin{ -3.14159265f };            // limite inferior da junta
    float jointMax{ 3.14159265f };             // limite superior da junta
    float jointAngle{ 0.0f };                  // valor inicial (rad/units)
};

// Configuração (all-or-nothing: fora do range é recusado, nunca clampeado).
struct MultibodyConfig {
    std::size_t maxLinks{ 16 };    // cap de links [1, 64]
    int solverIterations{ 8 };     // iterações de projeção pós-integração [1, 64]
    float damping{ 0.05f };        // amortecimento de junta [0, 1)
    glm::vec3 gravity{ 0.0f, -9.8f, 0.0f };

    bool valid(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;
};

// Estado observável de um link após um step.
struct MultibodyLinkState {
    glm::vec3 position{ 0.0f, 0.0f, 0.0f };   // COM no frame do mundo
    glm::vec3 velocity{ 0.0f, 0.0f, 0.0f };
    glm::quat orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
    float jointAngle{ 0.0f };                  // q da junta
    float jointVelocity{ 0.0f };               // q̇ da junta
};

using MultibodyHandle = std::uint64_t;
constexpr MultibodyHandle InvalidMultibody = 0;

class IMultibodyDynamics {
public:
    virtual ~IMultibodyDynamics() = default;

    virtual bool configure(const MultibodyConfig& config,
                           std::string& errorOut) = 0;
    virtual const MultibodyConfig& config() const noexcept = 0;

    // Cria uma cadeia: `root` é ancorado (fixo no mundo); cada desc seguinte
    // articula ao último link criado. Recusa (false + diagnóstico) uma
    // cadeia vazia, massa/inércia <= 0, limite de junta invertido, jointAxis
    // nulo ou nº de links acima de maxLinks. Retorna o handle da cadeia.
    virtual MultibodyHandle create_chain(
        const std::vector<MultibodyLinkDesc>& links,
        std::string& errorOut) = 0;
    virtual bool destroy_chain(MultibodyHandle chain) = 0;
    virtual std::size_t chain_count() const noexcept = 0;

    // Avança todas as cadeias: monta M(q), resolve q̈ = M⁻¹(τ − g) com
    // τ = 0 (nesta versão a dinâmica integra gravidade, damping e limites de
    // junta; torques externos por junta entram em evolução futura), integra
    // q̇/q semi-implícito e aplica damping + limites. Determinístico. dt > 0.
    virtual void step(float dt) = 0;

    // Observabilidade.
    virtual std::size_t link_count(MultibodyHandle chain) const noexcept = 0;
    virtual MultibodyLinkState link_state(MultibodyHandle chain,
                                          std::size_t link) const noexcept = 0;
};

// Fábrica do adapter (o único TU que implementa IMultibodyDynamics).
std::unique_ptr<IMultibodyDynamics> create_multibody_dynamics(
    std::string& errorOut);
std::unique_ptr<IMultibodyDynamics> create_multibody_dynamics_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace physics
}  // namespace engine
