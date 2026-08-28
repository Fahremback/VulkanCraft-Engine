// FlightDynamicsTests — gate do contrato público de dinâmica de voo
// (IFlightDynamics). Fecha o item G.jsbsim: a contraparte headless/
// determinística do modelo de coeficientes aerodinâmicos do catálogo,
// implementada do zero no SDK, sem PkgConfig/Cython/Python3.
//
// Prova: condição de trim (sustentação = peso na velocidade de equilíbrio),
// queda livre sob gravidade, estabilidade de arfagem (momento restaurador),
// determinismo bit-exact e validação all-or-nothing.

#include "engine/vehicles/IFlightDynamics.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

constexpr float kG = 9.80665f;

using engine::vehicles::AircraftSpec;
using engine::vehicles::create_flight_dynamics;
using engine::vehicles::create_flight_dynamics_json;
using engine::vehicles::FlightControls;
using engine::vehicles::FlightState;

void test_validation_and_factory() {
    AircraftSpec spec;
    std::string error;
    auto fd = create_flight_dynamics(error);
    check(fd != nullptr, "factory ok");

    AircraftSpec bad = spec;
    bad.mass = 0.0f;
    check(!bad.valid(error) && !error.empty(), "mass 0 refused");

    bad = spec;
    bad.wingArea = 0.0f;
    check(!bad.valid(error) && !error.empty(), "wingArea 0 refused");

    bad = spec;
    bad.clAlpha = 0.0f;
    check(!bad.valid(error) && !error.empty(), "clAlpha 0 refused");

    bad = spec;
    bad.cmAlpha = 0.5f;  // positivo = instável
    check(!bad.valid(error) && !error.empty(), "positive cmAlpha refused");

    bad = spec;
    bad.oswald = 0.0f;
    check(!bad.valid(error) && !error.empty(), "oswald 0 refused");

    error.clear();
    auto json = create_flight_dynamics_json(
        R"({"mass":500,"wingArea":12,"thrust":3000})", error);
    check(json != nullptr && error.empty(), "json factory ok");
    check(std::fabs(json->spec().mass - 500.0f) < 1e-6f &&
              std::fabs(json->spec().wingArea - 12.0f) < 1e-6f &&
              std::fabs(json->spec().thrust - 3000.0f) < 1e-6f,
          "json config applied");
    auto badJson = create_flight_dynamics_json(R"({"mass":0})", error);
    check(badJson == nullptr && !error.empty(), "json invalid refused");
}

void test_trim() {
    AircraftSpec spec;
    std::string error;
    auto fd = create_flight_dynamics(error);
    check(fd->configure(spec, error), "configure ok");

    // Velocidade de trim: L = qbar·S·(cl0) = m·g  (α = 0).
    const float V = std::sqrt(2.0f * spec.mass * kG /
                              (spec.densitySL * spec.wingArea * spec.cl0));
    // z = 0: a atmosfera exponencial devolve exatamente densitySL, casando
    // com a velocidade de trim calculada acima.
    FlightState state;
    state.position = glm::vec3(0.0f, 0.0f, 0.0f);
    state.velocity = glm::vec3(V, 0.0f, 0.0f);
    state.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    state.angularVelocity = glm::vec3(0.0f);
    fd->reset(state);

    FlightControls controls;  // tudo neutro
    fd->step(0.02f, controls);

    // Corpo: z é "para cima". Em trim, aceleração vertical ~ 0.
    const float az = fd->body_acceleration().z;
    check(std::fabs(az) < 1e-3f * kG,
          "trim: vertical body acceleration ~ 0");
    // Sem momento de arfagem: velocidade angular ~ 0.
    check(std::fabs(fd->state().angularVelocity.y) < 1e-6f,
          "trim: no pitch rate accumulation");
    check(std::fabs(fd->alpha()) < 1e-6f, "trim: alpha ~ 0");
}

void test_gravity_drop() {
    AircraftSpec spec;
    spec.thrust = 0.0f;
    std::string error;
    auto fd = create_flight_dynamics(error);
    check(fd->configure(spec, error), "configure ok");

    FlightState state;
    state.position = glm::vec3(0.0f, 0.0f, 100.0f);
    state.velocity = glm::vec3(0.0f, 0.0f, 0.0f);
    state.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    fd->reset(state);
    fd->step(0.02f, FlightControls());

    // Queda livre: a_body ≈ (0, 0, -g) no corpo (atitudes nivelada).
    const glm::vec3 a = fd->body_acceleration();
    check(std::fabs(a.z + kG) < 1e-3f * kG, "free fall: a_z = -g");
    check(std::fabs(a.x) < 1e-3f * kG && std::fabs(a.y) < 1e-3f * kG,
          "free fall: no lateral acceleration");
}

void test_pitch_stability() {
    AircraftSpec spec;
    std::string error;
    auto fd = create_flight_dynamics(error);
    check(fd->configure(spec, error), "configure ok");

    // Voo nivelado com nariz acima (α > 0): cmAlpha < 0 gera momento de
    // arfagem negativo (restaurador).
    FlightState state;
    state.position = glm::vec3(0.0f, 0.0f, 500.0f);
    state.velocity = glm::vec3(50.0f, 0.0f, 5.0f);  // α ≈ 0.1 rad
    state.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    fd->reset(state);
    fd->step(0.02f, FlightControls());
    check(fd->alpha() > 0.05f, "alpha positive with nose up");
    check(fd->state().angularVelocity.y < 0.0f,
          "positive alpha produces a restoring (negative) pitch rate");
}

void test_determinism() {
    AircraftSpec spec;
    std::string error;
    auto a = create_flight_dynamics(error);
    auto b = create_flight_dynamics(error);
    check(a->configure(spec, error) && b->configure(spec, error),
          "configure both ok");
    FlightControls controls;
    controls.elevator = 0.1f;
    controls.throttle = 0.7f;
    FlightState st;
    st.position = glm::vec3(0.0f, 0.0f, 300.0f);
    st.velocity = glm::vec3(40.0f, 0.0f, 2.0f);
    a->reset(st);
    b->reset(st);
    for (int i = 0; i < 40; ++i) {
        a->step(0.02f, controls);
        b->step(0.02f, controls);
    }
    check(a->state().position == b->state().position &&
              a->state().velocity == b->state().velocity &&
              a->state().orientation == b->state().orientation,
          "two identical sims step bit-identically");
}

}  // namespace

int main() {
    test_validation_and_factory();
    test_trim();
    test_gravity_drop();
    test_pitch_stability();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "flight_dynamics_tests: all checks passed\n";
        return 0;
    }
    std::cout << "flight_dynamics_tests: " << g_failures << " failure(s)\n";
    return 1;
}
