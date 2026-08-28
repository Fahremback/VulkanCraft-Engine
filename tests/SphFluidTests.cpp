// SphFluidTests — gate do contrato público de fluido SPH
// (ISPHFluidSimulation). Fecha o item G.splishsplash: a contraparte
// headless/determinística do solver WCSPH do catálogo, implementada do zero
// no SDK, sem zlib/CompactNSearch/Discregrid.
//
// Prova: densidade aumenta com a proximidade, repulsão quando comprimido,
// assentamento no piso com gravidade, conservação de massa, determinismo
// bit-exact, validação all-or-nothing e factory JSON.

#include "engine/simulation/ISPHFluidSimulation.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

using engine::simulation::create_sph_fluid_simulation;
using engine::simulation::create_sph_fluid_simulation_json;
using engine::simulation::SphFluidConfig;

void test_validation_and_factory() {
    SphFluidConfig config;
    std::string error;
    auto sim = create_sph_fluid_simulation(error);
    check(sim != nullptr, "factory creates the solver");

    SphFluidConfig bad = config;
    bad.maxParticles = 0;
    check(!bad.valid(error) && !error.empty(), "maxParticles 0 refused");

    bad = config;
    bad.particleRadius = 0.0f;
    check(!bad.valid(error) && !error.empty(), "particleRadius 0 refused");

    bad = config;
    bad.restDensity = 0.0f;
    check(!bad.valid(error) && !error.empty(), "restDensity 0 refused");

    bad = config;
    bad.damping = 1.0f;
    check(!bad.valid(error) && !error.empty(), "damping 1 refused");

    error.clear();
    auto json = create_sph_fluid_simulation_json(
        R"({"maxParticles":512,"particleRadius":0.2,"viscosity":0.1})", error);
    check(json != nullptr && error.empty(), "json factory ok");
    check(json->config().maxParticles == 512 &&
              std::fabs(json->config().particleRadius - 0.2f) < 1e-6f &&
              std::fabs(json->config().viscosity - 0.1f) < 1e-6f,
          "json config applied");

    auto badJson = create_sph_fluid_simulation_json(
        R"({"maxParticles":0})", error);
    check(badJson == nullptr && !error.empty(), "json invalid refused");
}

void test_density_and_repulsion() {
    SphFluidConfig config;
    config.particleRadius = 0.1f;
    std::string error;
    auto sim = create_sph_fluid_simulation(error);

    // Densidade sobe conforme os pares se aproximam.
    // h = 4·raio = 0.4 (suporte do kernel); 0.8 está fora, 0.3 dentro.
    const glm::vec3 a(0.0f, 2.0f, 0.0f);
    const glm::vec3 bFar(0.0f, 2.0f, 0.8f);
    const glm::vec3 bNear(0.0f, 2.0f, 0.3f);
    std::vector<glm::vec3> pos{ a, bFar };
    std::vector<glm::vec3> vel{ glm::vec3(0.0f), glm::vec3(0.0f) };
    check(sim->configure(config, error) && sim->reset(pos, vel, error),
          "far pair reset ok");
    sim->step(0.016f);
    const float rhoFar = sim->particle_density(0);
    pos[1] = bNear;
    check(sim->reset(pos, vel, error), "near pair reset ok");
    sim->step(0.016f);
    const float rhoNear = sim->particle_density(0);
    check(rhoNear > rhoFar, "density increases as particles approach");

    // Par cruzando a origem (células -1 e 0): a busca de vizinhos deve
    // achar o par mesmo com coordenadas de célula negativas (decode do
    // pacote com bias).
    pos[0] = glm::vec3(-0.09f, 2.0f, 0.0f);
    pos[1] = glm::vec3(0.09f, 2.0f, 0.0f);
    check(sim->reset(pos, vel, error), "origin-straddling pair reset ok");
    sim->step(0.016f);
    const float rhoStraddle = sim->particle_density(0);
    check(rhoStraddle > rhoFar,
          "neighbor found across the negative/positive cell boundary");

    // Repulsão: par comprimido (distância 0.4r < espaçamento de repouso)
    // afasta após vários steps (pressão de estado empurra).
    pos[0] = glm::vec3(0.0f, 2.0f, 0.0f);
    pos[1] = glm::vec3(0.0f, 2.0f, 0.4f * config.particleRadius + 1e-4f);
    vel[0] = glm::vec3(0.0f);
    vel[1] = glm::vec3(0.0f);
    check(sim->reset(pos, vel, error), "squeezed pair reset ok");
    for (int i = 0; i < 30; ++i) sim->step(0.016f);
    const float sep = glm::distance(sim->particle_position(0),
                                    sim->particle_position(1));
    const float initial = glm::distance(pos[0], pos[1]);
    check(sep > initial * 1.5f, "compressed pair repels apart");
}

void test_gravity_settling_and_mass() {
    SphFluidConfig config;
    config.particleRadius = 0.1f;
    std::string error;
    auto sim = create_sph_fluid_simulation(error);
    check(sim->configure(config, error), "configure ok");

    // 3x3x3 bloco acima do piso: cai e assenta (y -> groundY + raio).
    std::vector<glm::vec3> pos;
    std::vector<glm::vec3> vel;
    const float spacing = 2.0f * config.particleRadius;
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                pos.emplace_back(static_cast<float>(x) * spacing,
                                 2.0f + static_cast<float>(y) * spacing,
                                 static_cast<float>(z) * spacing);
                vel.emplace_back(0.0f);
            }
    check(sim->reset(pos, vel, error), "block reset ok");
    const float massBefore = sim->total_mass();
    check(massBefore > 0.0f, "total mass positive");

    for (int i = 0; i < 900; ++i) sim->step(0.016f);

    check(std::fabs(sim->total_mass() - massBefore) < 1e-3f,
          "total mass conserved during the run");
    float minY = 1e9f;
    float speedSum = 0.0f;
    for (std::size_t i = 0; i < sim->particle_count(); ++i) {
        minY = std::min(minY, sim->particle_position(i).y);
        speedSum += glm::length(sim->particle_velocity(i));
    }
    check(std::fabs(minY - (config.groundY + config.particleRadius)) < 1e-4f,
          "particles settle on the ground (y = groundY + radius)");
    // Assentamento: as partículas internas da poa oscilam em torno da
    // posição de repouso, mas a energia cinética média decai (damping +
    // piso com restituição 0); medimos a velocidade média, não a máxima.
    const float avgSpeed = speedSum / static_cast<float>(sim->particle_count());
    check(avgSpeed < 0.05f, "settled particles are nearly at rest");
}

void test_determinism() {
    SphFluidConfig config;
    config.particleRadius = 0.1f;
    std::string error;
    auto a = create_sph_fluid_simulation(error);
    auto b = create_sph_fluid_simulation(error);
    check(a->configure(config, error) && b->configure(config, error),
          "configure both ok");

    std::vector<glm::vec3> pos;
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) {
                pos.emplace_back(static_cast<float>(x) * 0.2f,
                                 1.0f + static_cast<float>(y) * 0.2f,
                                 static_cast<float>(z) * 0.2f);
            }
    std::vector<glm::vec3> vel(pos.size(), glm::vec3(0.0f));
    check(a->reset(pos, vel, error) && b->reset(pos, vel, error),
          "reset both ok");
    for (int i = 0; i < 50; ++i) {
        a->step(0.016f);
        b->step(0.016f);
    }
    bool identical = true;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        if (a->particle_position(i) != b->particle_position(i))
            identical = false;
    }
    check(identical, "two identical sims produce bit-identical states");
}

}  // namespace

int main() {
    test_validation_and_factory();
    test_density_and_repulsion();
    test_gravity_settling_and_mass();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "sph_fluid_tests: all checks passed\n";
        return 0;
    }
    std::cout << "sph_fluid_tests: " << g_failures << " failure(s)\n";
    return 1;
}
