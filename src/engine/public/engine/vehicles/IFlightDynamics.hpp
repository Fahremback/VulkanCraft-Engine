// IFlightDynamics.hpp
//
// PUBLIC seam para dinâmica de voo de aeronaves de asa fixa (6-DOF). É a
// contraparte headless/determinística do catálogo jsbsim (flight dynamics) —
// implementada do zero no SDK, sem dependência externa, com o modelo
// clássico de coeficientes aerodinâmicos:
//   - atmosfera: densidade exponencial com a altitude;
//   - aero: CL = cl0 + clAlpha·α, CD = cd0 + k·CL², CM = cmAlpha·α +
//     cmElevator·δe; forças em eixo estabilidade convertidas para o corpo;
//   - propulsão: empuxo ao longo de +x corpo;
//   - gravidade + inércia rotacional; integração semi-implícita de Euler com
//     quaternion de atitude normalizado.
//
// Escopo: o ALGORITMO de voo (aeromodelos, veículos aéreos, mísseis,
// gameplay de aviação) — headless, determinístico, consumível pelo provider
// de veículos e por simulações. NÃO substitui o Jolt; é um modelo de
// coeficientes, não um resolvedor de corpo rígido genérico.
//
// Determinismo: sem RNG, ordem fixa de avaliação — mesma (spec, estado,
// controles, dt) produz o mesmo próximo estado (bit-exact).

#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>

namespace engine {
namespace vehicles {

// Especificação da aeronave (all-or-nothing: valores fora do range são
// recusados com diagnóstico, nunca clampeados).
struct AircraftSpec {
    float mass{ 1000.0f };          // kg, > 0
    glm::vec3 inertia{ 2000.0f, 4000.0f, 2000.0f };  // Ixx, Iyy, Izz (> 0)
    float wingArea{ 16.0f };        // S (m², > 0)
    float wingSpan{ 12.0f };        // b (m, > 0)
    float cl0{ 0.25f };             // CL em α = 0
    float clAlpha{ 4.5f };          // dCL/dα por radiano (> 0)
    float cd0{ 0.03f };             // CD parasita (>= 0)
    float oswald{ 0.8f };           // eficiência de Oswald (0, 1]
    float cmAlpha{ -0.5f };         // dCM/dα (estabilidade: < 0)
    float cmElevator{ 0.8f };       // dCM/dδe (> 0)
    float maxElevator{ 0.35f };     // curso máximo do profundor (rad, > 0)
    float clAileron{ 0.2f };        // dCl/dδa (rolagem, > 0)
    float cnRudder{ 0.1f };         // dCn/dδr (guinada, > 0)
    float thrust{ 5000.0f };        // empuxo máximo (N, >= 0)
    float densitySL{ 1.225f };      // densidade ao nível do mar (> 0)
    float densityScale{ 1.0f / 8500.0f };  // decaimento exponencial (1/m, > 0)

    bool valid(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;
};

// Estado de voo: posição no mundo (z up — altitude = position.z), velocidade
// no CORPO (x = nariz, z = para cima), atitude e velocidade angular no corpo.
struct FlightState {
    glm::vec3 position{ 0.0f, 0.0f, 100.0f };  // z = altitude
    glm::vec3 velocity{ 50.0f, 0.0f, 0.0f };   // corpo (x = nariz)
    glm::quat orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 angularVelocity{ 0.0f, 0.0f, 0.0f };
};

struct FlightControls {
    float elevator{ 0.0f };   // [-1, 1]
    float aileron{ 0.0f };    // [-1, 1]
    float rudder{ 0.0f };     // [-1, 1]
    float throttle{ 1.0f };   // [0, 1]
};

class IFlightDynamics {
public:
    virtual ~IFlightDynamics() = default;

    virtual bool configure(const AircraftSpec& spec, std::string& errorOut) = 0;
    virtual const AircraftSpec& spec() const noexcept = 0;

    virtual void reset(const FlightState& state) noexcept = 0;
    virtual const FlightState& state() const noexcept = 0;

    // Avança um passo: atmosfera, forças aero/gravidade/empuxo, momentos,
    // integração de corpo rígido, quaternion normalizado. `dt` > 0. Controles
    // são clampados aos limites de curso.
    virtual void step(float dt, const FlightControls& controls) = 0;

    // Observabilidade: aceleração no corpo do último step e ângulo de ataque
    // (rad) avaliado no último step.
    virtual glm::vec3 body_acceleration() const noexcept = 0;
    virtual float alpha() const noexcept = 0;
};

// Fábrica do adapter (o único TU que implementa IFlightDynamics).
std::unique_ptr<IFlightDynamics> create_flight_dynamics(std::string& errorOut);
std::unique_ptr<IFlightDynamics> create_flight_dynamics_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace vehicles
}  // namespace engine
