// FlightDynamics.cpp — the only TU implementing IFlightDynamics.
//
// Dinâmica de voo de asa fixa (6-DOF) do zero, sem dependência externa:
// atmosfera exponencial, coeficientes aerodinâmicos CL/CD/CM, forças em
// eixo estabilidade convertidas para o corpo, empuxo, gravidade, momentos
// (pitch/roll/yaw) e integração semi-implícita de Euler com quaternion
// normalizado. Mesmo modelo conceitual do jsbsim (coeficientes), headless e
// determinístico (sem RNG, ordem fixa).

#include "engine/vehicles/IFlightDynamics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace engine {
namespace vehicles {
namespace {

constexpr float kG = 9.80665f;
constexpr float kPi = 3.14159265358979323846f;

bool AircraftSpec_validate(const AircraftSpec& spec, std::string& errorOut) {
    if (!(spec.mass > 0.0f) || !std::isfinite(spec.mass)) {
        errorOut = "flight spec: mass must be finite and > 0";
        return false;
    }
    if (!(spec.inertia.x > 0.0f && spec.inertia.y > 0.0f &&
          spec.inertia.z > 0.0f)) {
        errorOut = "flight spec: inertia must be > 0 on all axes";
        return false;
    }
    if (!(spec.wingArea > 0.0f) || !(spec.wingSpan > 0.0f)) {
        errorOut = "flight spec: wingArea and wingSpan must be > 0";
        return false;
    }
    if (!(spec.clAlpha > 0.0f)) {
        errorOut = "flight spec: clAlpha must be > 0";
        return false;
    }
    if (spec.cd0 < 0.0f) {
        errorOut = "flight spec: cd0 must be >= 0";
        return false;
    }
    if (!(spec.oswald > 0.0f && spec.oswald <= 1.0f)) {
        errorOut = "flight spec: oswald must be in (0, 1]";
        return false;
    }
    if (!(spec.cmAlpha < 0.0f)) {
        errorOut = "flight spec: cmAlpha must be < 0 (pitch stability)";
        return false;
    }
    if (spec.cmElevator <= 0.0f || spec.clAileron <= 0.0f ||
        spec.cnRudder <= 0.0f) {
        errorOut = "flight spec: control coefficients must be > 0";
        return false;
    }
    if (!(spec.maxElevator > 0.0f) || !(spec.thrust >= 0.0f)) {
        errorOut = "flight spec: maxElevator > 0 and thrust >= 0 required";
        return false;
    }
    if (!(spec.densitySL > 0.0f) || !(spec.densityScale > 0.0f)) {
        errorOut = "flight spec: atmosphere constants must be > 0";
        return false;
    }
    return true;
}

// Rotação quaternion * vetor (v é body -> world).
inline glm::vec3 quat_rotate(const glm::quat& q, const glm::vec3& v) {
    return q * v;
}

class FlightDynamics final : public IFlightDynamics {
public:
    const AircraftSpec& spec() const noexcept override { return spec_; }

    bool configure(const AircraftSpec& spec, std::string& errorOut) override {
        if (!AircraftSpec_validate(spec, errorOut)) return false;
        spec_ = spec;
        return true;
    }

    void reset(const FlightState& state) noexcept override {
        state_ = state;
        accel_ = glm::vec3(0.0f);
        alpha_ = 0.0f;
    }

    const FlightState& state() const noexcept override { return state_; }

    void step(float dt, const FlightControls& controls) override {
        if (!(dt > 0.0f)) return;

        // Clamp dos controles aos cursos.
        const float de = glm::clamp(controls.elevator, -1.0f, 1.0f) *
                         spec_.maxElevator;
        const float da = glm::clamp(controls.aileron, -1.0f, 1.0f);
        const float dr = glm::clamp(controls.rudder, -1.0f, 1.0f);
        const float throttle = glm::clamp(controls.throttle, 0.0f, 1.0f);

        // Atmosfera exponencial (mundo z-up: altitude = position.z).
        const float rho =
            spec_.densitySL *
            std::exp(-std::max(0.0f, state_.position.z) * spec_.densityScale);

        // Velocidade no corpo e ângulos.
        const glm::vec3 v = state_.velocity;
        const float V = glm::length(v);
        alpha_ = V > 1e-6f ? std::atan2(v.z, v.x) : 0.0f;
        const float qbar = 0.5f * rho * V * V;
        const float AR = spec_.wingSpan * spec_.wingSpan / spec_.wingArea;
        const float kDrag = 1.0f / (kPi * spec_.oswald * AR);
        const float CL = spec_.cl0 + spec_.clAlpha * alpha_;
        const float CD = spec_.cd0 + kDrag * CL * CL;

        // Forças aerodinâmicas no corpo (eixo estabilidade convertido).
        glm::vec3 aero(0.0f);
        if (V > 1e-6f) {
            const glm::vec3 dir = v / V;
            // arrasto ao longo de -v; sustentação ⊥ v no plano de arfagem.
            glm::vec3 liftDir = glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f));
            const float liftLen = glm::length(liftDir);
            if (liftLen > 1e-6f) {
                liftDir = liftDir / liftLen;
                aero = -dir * (qbar * spec_.wingArea * CD) +
                       liftDir * (qbar * spec_.wingArea * CL);
            } else {
                aero = -dir * (qbar * spec_.wingArea * CD);
            }
        }

        // Empuxo (ao longo de +x corpo) e gravidade (world -> body). Mundo
        // z-up e corpo z-up: em atitude nivelada a gravidade é -z no corpo.
        const glm::vec3 thrust(spec_.thrust * throttle, 0.0f, 0.0f);
        const glm::mat3 rot = glm::mat3_cast(state_.orientation);
        const glm::vec3 gravityBody =
            glm::transpose(rot) * glm::vec3(0.0f, 0.0f, -kG);

        const glm::vec3 force = aero + thrust + gravityBody * spec_.mass;
        accel_ = force / spec_.mass;

        // Momentos: pitch (cmAlpha·α + cmElevator·δe), roll (aileron),
        // yaw (rudder). Chord médio = S/b.
        const float chord = spec_.wingArea / spec_.wingSpan;
        const glm::vec3 moments(
            qbar * spec_.wingArea * spec_.wingSpan * spec_.clAileron * da,
            qbar * spec_.wingArea * chord *
                (spec_.cmAlpha * alpha_ + spec_.cmElevator * de),
            qbar * spec_.wingArea * spec_.wingSpan * spec_.cnRudder * dr);

        // Integração semi-implícita (velocidades primeiro). state_.velocity é
        // a velocidade no FRAME DO CORPO, então a dinâmica translacional
        // precisa do termo de transporte rotacional: dv/dt = F/m − ω×v. Sem
        // ele, em rotações fortes a velocidade corporal é transformada de
        // forma fisicamente inconsistente (a taxa de variação de um vetor
        // medido no frame girante inclui −ω×v).
        const glm::vec3 coriolis =
            glm::cross(state_.angularVelocity, state_.velocity);
        state_.velocity += (accel_ - coriolis) * dt;
        const glm::vec3 I(spec_.inertia.x, spec_.inertia.y, spec_.inertia.z);
        const glm::vec3 gyro = glm::cross(state_.angularVelocity, I * state_.angularVelocity);
        const glm::vec3 angAcc = (moments - gyro) / I;
        state_.angularVelocity += angAcc * dt;

        // Posição (velocidade no corpo -> mundo) e atitude (quaternion).
        state_.position += quat_rotate(state_.orientation, state_.velocity) * dt;
        const glm::quat qdot = 0.5f * state_.orientation *
                               glm::quat(0.0f, state_.angularVelocity.x,
                                         state_.angularVelocity.y,
                                         state_.angularVelocity.z);
        state_.orientation = glm::normalize(state_.orientation + qdot * dt);
    }

    glm::vec3 body_acceleration() const noexcept override { return accel_; }
    float alpha() const noexcept override { return alpha_; }

private:
    AircraftSpec spec_;
    FlightState state_;
    glm::vec3 accel_{ 0.0f };
    float alpha_{ 0.0f };
};

}  // namespace

bool AircraftSpec::valid(std::string& errorOut) const {
    return AircraftSpec_validate(*this, errorOut);
}

bool AircraftSpec::load_from_json(const std::string& json,
                                  std::string& errorOut) {
    AircraftSpec candidate = *this;
    bool any = false;
    std::size_t pos = 0;
    while (pos < json.size()) {
        const std::size_t kStart = json.find('"', pos);
        if (kStart == std::string::npos) break;
        const std::size_t kEnd = json.find('"', kStart + 1);
        if (kEnd == std::string::npos) break;
        const std::string key = json.substr(kStart + 1, kEnd - kStart - 1);
        const std::size_t colon = json.find(':', kEnd);
        if (colon == std::string::npos) break;
        const std::size_t vStart = json.find_first_not_of(" \t\r\n", colon + 1);
        if (vStart == std::string::npos) break;
        const std::size_t vEnd = json.find_first_of(",}", vStart);
        const std::string value =
            json.substr(vStart, vEnd == std::string::npos ? std::string::npos
                                                          : vEnd - vStart);
        float* f = nullptr;
        if (key == "mass") f = &candidate.mass;
        else if (key == "wingArea") f = &candidate.wingArea;
        else if (key == "wingSpan") f = &candidate.wingSpan;
        else if (key == "cl0") f = &candidate.cl0;
        else if (key == "clAlpha") f = &candidate.clAlpha;
        else if (key == "cd0") f = &candidate.cd0;
        else if (key == "oswald") f = &candidate.oswald;
        else if (key == "cmAlpha") f = &candidate.cmAlpha;
        else if (key == "cmElevator") f = &candidate.cmElevator;
        else if (key == "maxElevator") f = &candidate.maxElevator;
        else if (key == "clAileron") f = &candidate.clAileron;
        else if (key == "cnRudder") f = &candidate.cnRudder;
        else if (key == "thrust") f = &candidate.thrust;
        else if (key == "densitySL") f = &candidate.densitySL;
        else if (key == "densityScale") f = &candidate.densityScale;
        if (f != nullptr) {
            *f = std::strtof(value.c_str(), nullptr);
            any = true;
        } else if (key == "inertia") {
            // Array [x, y, z] inline — parse simples dos 3 floats.
            const std::size_t a = json.find('[', vStart);
            const std::size_t b = json.find(']', vStart);
            if (a != std::string::npos && b != std::string::npos) {
                std::string inner = json.substr(a + 1, b - a - 1);
                std::size_t p1 = inner.find(',');
                std::size_t p2 = p1 == std::string::npos ? std::string::npos
                                                         : inner.find(',', p1 + 1);
                candidate.inertia.x = std::strtof(inner.substr(0, p1).c_str(), nullptr);
                if (p2 != std::string::npos) {
                    candidate.inertia.y = std::strtof(
                        inner.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr);
                    candidate.inertia.z = std::strtof(
                        inner.substr(p2 + 1).c_str(), nullptr);
                }
            }
            any = true;
        }
        pos = vEnd == std::string::npos ? json.size() : vEnd + 1;
    }
    if (!any) {
        errorOut = "flight spec: no recognized keys";
        return false;
    }
    if (!AircraftSpec_validate(candidate, errorOut)) return false;
    *this = candidate;
    return true;
}

std::string AircraftSpec::to_json() const {
    std::string out = "{";
    out += "\"mass\":" + std::to_string(mass) + ",";
    out += "\"inertia\":[" + std::to_string(inertia.x) + "," +
           std::to_string(inertia.y) + "," +
           std::to_string(inertia.z) + "],";
    out += "\"wingArea\":" + std::to_string(wingArea) + ",";
    out += "\"wingSpan\":" + std::to_string(wingSpan) + ",";
    out += "\"cl0\":" + std::to_string(cl0) + ",";
    out += "\"clAlpha\":" + std::to_string(clAlpha) + ",";
    out += "\"cd0\":" + std::to_string(cd0) + ",";
    out += "\"oswald\":" + std::to_string(oswald) + ",";
    out += "\"cmAlpha\":" + std::to_string(cmAlpha) + ",";
    out += "\"cmElevator\":" + std::to_string(cmElevator) + ",";
    out += "\"maxElevator\":" + std::to_string(maxElevator) + ",";
    out += "\"clAileron\":" + std::to_string(clAileron) + ",";
    out += "\"cnRudder\":" + std::to_string(cnRudder) + ",";
    out += "\"thrust\":" + std::to_string(thrust) + ",";
    out += "\"densitySL\":" + std::to_string(densitySL) + ",";
    out += "\"densityScale\":" + std::to_string(densityScale);
    out += "}";
    return out;
}

std::unique_ptr<IFlightDynamics> create_flight_dynamics(
    std::string& errorOut) {
    auto impl = std::make_unique<FlightDynamics>();
    if (!impl) {
        errorOut = "flight: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IFlightDynamics> create_flight_dynamics_json(
    const std::string& jsonText, std::string& errorOut) {
    AircraftSpec spec;
    if (!spec.load_from_json(jsonText, errorOut)) return nullptr;
    auto impl = std::make_unique<FlightDynamics>();
    if (!impl->configure(spec, errorOut)) return nullptr;
    return impl;
}

}  // namespace vehicles
}  // namespace engine
