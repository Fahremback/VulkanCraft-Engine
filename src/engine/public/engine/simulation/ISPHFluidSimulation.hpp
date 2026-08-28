// ISPHFluidSimulation.hpp
//
// PUBLIC seam para simulação de fluidos sem malha (Smoothed Particle
// Hydrodynamics). É a contraparte headless/determinística do catálogo
// splishsplash (WCSPH) — implementada do zero no SDK, sem qualquer
// dependência externa, seguindo a mesma família de algoritmos (densidade
// via kernel, pressão de estado, gradiente de pressão + viscosidade).
//
// Escopo: o ALGORITMO de fluido (partículas, vizinhança em grade espacial,
// densidade/pressão/forças, integração) — headless, determinístico e
// consumível por previews de água/lava, efeitos e física de partículas.
// NÃO é o fluid solver de águas rasas (heightfield) — esse é o
// IFluidSimulation de rendering; este contrato é o SPH volumétrico.
//
// Determinismo: a grade espacial de vizinhança percorre células em ordem
// lexicográfica fixa e as partículas em ordem de inserção; o RNG interno é
// splitmix64 semeado (sem RNG global/relógio). A mesma sequência de steps
// com o mesmo estado produz resultados bit-exact.

#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace simulation {

// Configuração do solver WCSPH. Toda configuração é all-or-nothing: valores
// fora do range são RECUSADOS com diagnóstico (nunca clampeados), seguindo a
// convenção dos demais contratos.
struct SphFluidConfig {
    std::uint32_t maxParticles{ 4096 };   // cap de partículas [1, 1<<20]
    float particleRadius{ 0.1f };         // raio de partícula (m, > 0)
    float restDensity{ 1000.0f };         // densidade de repouso (> 0)
    float stiffness{ 500.0f };            // pressão de estado k (> 0)
    float viscosity{ 0.05f };             // viscosidade (>= 0)
    float gravity{ -9.81f };              // aceleração (m/s^2, eixo Y)
    float damping{ 0.01f };               // amortecimento global [0, 1)
    float groundY{ 0.0f };                // piso (clamp + restituição 0)

    // JSON keys: maxParticles / particleRadius / restDensity / stiffness /
    // viscosity / gravity / damping / groundY. All-or-nothing.
    bool valid(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;
};

// Estado observável de uma partícula após um step.
struct SphParticleState {
    glm::vec3 position{ 0.0f, 0.0f, 0.0f };
    glm::vec3 velocity{ 0.0f, 0.0f, 0.0f };
    float density{ 0.0f };    // densidade do kernel na última avaliação
    float pressure{ 0.0f };   // pressão de estado (Tait/linear) na última avaliação
};

class ISPHFluidSimulation {
public:
    virtual ~ISPHFluidSimulation() = default;

    // All-or-nothing: aplica a config (recusada se inválida) e esvazia o
    // sistema.
    virtual bool configure(const SphFluidConfig& config,
                           std::string& errorOut) = 0;
    virtual const SphFluidConfig& config() const noexcept = 0;

    // Reinicia o sistema com partículas nas posições/velocidades dadas.
    // Recusa (false + diagnóstico) um número acima de maxParticles ou vetores
    // de tamanho divergente. Estado de densidade/pressão é recalculado no
    // próximo step.
    virtual bool reset(const std::vector<glm::vec3>& positions,
                       const std::vector<glm::vec3>& velocities,
                       std::string& errorOut) = 0;

    // Avança o sistema: densidade (kernel poly6), pressão de estado,
    // acelerações (gradiente spiky + viscosidade laplaciana + gravidade),
    // integração semi-implícita de Euler, piso com restituição 0, damping.
    // Determinístico. dt deve ser > 0.
    virtual void step(float dt) = 0;

    // Observabilidade.
    virtual std::size_t particle_count() const noexcept = 0;
    virtual SphParticleState particle(std::size_t index) const noexcept = 0;
    virtual glm::vec3 particle_position(std::size_t index) const noexcept = 0;
    virtual glm::vec3 particle_velocity(std::size_t index) const noexcept = 0;
    virtual float particle_density(std::size_t index) const noexcept = 0;
    virtual float particle_pressure(std::size_t index) const noexcept = 0;

    // Massa total do sistema (n * massa por partícula) — conservada.
    virtual float total_mass() const noexcept = 0;
};

// Fábrica do adapter (o único TU que implementa ISPHFluidSimulation).
std::unique_ptr<ISPHFluidSimulation> create_sph_fluid_simulation(
    std::string& errorOut);
std::unique_ptr<ISPHFluidSimulation> create_sph_fluid_simulation_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace simulation
}  // namespace engine
