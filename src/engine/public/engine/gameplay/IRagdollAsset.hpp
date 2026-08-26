// IRagdollAsset — asset de ragdoll CONFIGURÁVEL (data-driven). Componente do
// §4 item 55 ("ragdoll configurável, joints, limits, drives, auto-balance e
// blending animação↔física"): o runtime físico (IRagdoll/ActiveRagdoll,
// FALTANTES item 9 / §18 item 8) já existe; falta a camada de AUTHORING —
// um skeleton com joints (nome/parent/anchor), limits (swing-twist), drives
// (motor position), auto-balance e configuração de blend, serializado como
// JSON versionado ALL-OR-NOTHING (mesmo padrão do IVehicleAsset).
//
// Este header é self-contained (engine//std/vendor only). load_from_json /
// to_json / validate / build_bones são implementados pelo adapter do SDK
// (src/engine/sdk/RagdollAsset.cpp). O asset é PURE DATA — nunca toca física.
// build_bones() mapeia para o RagdollBone público do runtime (o ponto de
// wiring: o chamador passa a lista ao create_ragdoll/ActiveRagdoll).

#pragma once

#include "engine/gameplay/IGameplayRuntime.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace gameplay {

// Limites swing-twist do joint (radianos). swingLimitX/swingLimitY limitam o
// cone de swing; twistLimit limita a rotação sobre o eixo do osso. 0 = eixo
// travado naquele grau de liberdade.
struct RagdollJointLimit {
    float swingLimitX{ 0.0f };
    float swingLimitY{ 0.0f };
    float twistLimit{ 0.0f };
};

// Motor de posição do joint (o motor swing-twist do Jolt, por trás do
// ActiveRagdoll::drive_to_pose). enabled=false = joint LIVRE (ragdoll
// parcial — a física manda); enabled=true = motor segura a pose de animação.
struct RagdollJointDrive {
    bool enabled{ false };
    float frequency{ 8.0f };  // > 0 quando enabled
    float damping{ 1.0f };    // >= 0
};

// Um joint do skeleton: o osso + a configuração do constraint.
struct RagdollJoint {
    std::string name;
    std::string parent;      // "" = raiz (pode haver várias raízes)
    glm::vec3 anchor{ 0.0f };  // offset local do parent (na raiz: posição do mundo)
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };  // pose base
    float length{ 0.5f };    // > 0
    float radius{ 0.12f };   // > 0
    float mass{ 1.0f };      // > 0
    RagdollJointLimit limits;
    RagdollJointDrive drive;
};

// Configuração do blend animação↔física (recuperação do ragdoll parcial).
struct RagdollBlend {
    float recoverRate{ 1.0f };  // avanço do blend weight por segundo (>= 0)
};

struct RagdollAsset {
    int version{ 1 };
    std::string name;             // não-vazio
    std::vector<RagdollJoint> joints;  // não-vazio; nomes únicos; parents conhecidos; sem ciclos
    bool autoBalance{ false };    // dirigir a raiz para manter o CoM sobre o suporte
    RagdollBlend blend;

    // All-or-nothing: qualquer problema (JSON malformado, versão ≠ 1,
    // joints vazio, nome duplicado, parent desconhecido, ciclo, valor
    // não-finito/fora de faixa) rejeita o doc INTEIRO e mantém o estado
    // anterior intacto.
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    // Bit-exact: to_json() round-tripa todos os campos (emissão %.9g).
    std::string to_json() const;
    bool validate(std::string& errorOut) const;

    // Mapeia o asset para o skeleton público do runtime (RagdollBone):
    // name/parent/anchor→position/rotation/length/radius/mass, na ordem dos
    // joints. Ponto de wiring com IRagdoll::create_ragdoll / ActiveRagdoll.
    std::vector<RagdollBone> build_bones() const;
};

}  // namespace gameplay
}  // namespace engine
