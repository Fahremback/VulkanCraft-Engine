#pragma once
// IConstraints — limites de articulação (constraints) por osso.
//
// Contrato público self-contained (std only) do §4 item 2 (unidade
// "constraints"). Sem dependência de core/registro — só os tipos vetoriais
// de IAnimCore. Aplica limites de ângulo em torno dos eixos LOCAIS do osso.
//
// Semântica determinística de `apply_constraint(id, pose)`:
//   para cada osso limitado: extrai a rotação em ângulos de Euler (XYZ,
//   Tait-Bryan), clampa CADA ângulo em [min,max] (±inf = sem limite) e
//   reconstrói o quaternion (Qx·Qy·Qz). Ossos sem limite permanecem
//   intactos. Ângulo fora do range é clampado no limite mais próximo.
//
// Escopo §4 item 2: IK (#215) + retargeting (#217) + constraints (esta
// unidade); resta adaptação ao terreno (unidade registrada).

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct JointLimit {
    std::string bone;
    // Limites em radianos em torno dos eixos locais (XYZ); ±inf = sem
    // limite no eixo. min <= max (validado).
    double min_x = -1.7976931348623157e308;
    double max_x = 1.7976931348623157e308;
    double min_y = -1.7976931348623157e308;
    double max_y = 1.7976931348623157e308;
    double min_z = -1.7976931348623157e308;
    double max_z = 1.7976931348623157e308;
};

// Constraints de articulação — clampa ângulos locais, sem estado além do
// registro de limites (serialização bit-exact).
class IConstraints {
public:
    virtual ~IConstraints() = default;

    // All-or-nothing: osso não vazio e único na MESMA constraint; limites
    // finitos ou ±inf; min <= max em cada eixo.
    virtual bool add_constraint(const std::string& constraintId,
                                const std::vector<JointLimit>& limits,
                                std::string& errorOut) = 0;

    virtual bool has_constraint(const std::string& constraintId) const = 0;

    // Aplica os limites à pose (ordem preservada); osso do limite ausente
    // na pose = erro all-or-nothing (nada aplicado).
    virtual std::vector<BonePose> apply_constraint(
        const std::string& constraintId, const std::vector<BonePose>& pose,
        std::string& errorOut) const = 0;

    // Registro completo serializado / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IConstraints).
std::unique_ptr<IConstraints> create_constraints();

}  // namespace engine::animation
