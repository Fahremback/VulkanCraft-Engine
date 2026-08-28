// SphFluidSimulation.cpp — the only TU implementing ISPHFluidSimulation.
//
// WCSPH (Weakly Compressible SPH) implementado do zero, sem dependência
// externa: densidade via kernel poly6, pressão de estado linear (Desbrun),
// gradiente de pressão com kernel spiky, viscosidade com kernel laplaciano,
// integração semi-implícita de Euler e piso com restituição 0.
//
// Determinismo: vizinhança via grade espacial com células percorridas em
// ordem lexicográfica fixa (chave int64 empacotada) e partículas na ordem de
// inserção; cada par é processado uma única vez (j > i). Sem RNG, sem
// threading — a mesma sequência de steps produz estados bit-exact.

#include "engine/simulation/ISPHFluidSimulation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <utility>

namespace engine {
namespace simulation {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Kernel poly6 (W). support radius h.
inline float poly6(float r2, float h, float h9) {
    if (r2 >= h * h || r2 <= 0.0f) return 0.0f;
    const float d2 = h * h - r2;
    return (315.0f / (64.0f * kPi * h9)) * d2 * d2 * d2;
}

// Gradiente spiky (aponta para longe de j): ∇W = -45/(π h^6) (h - r)^2 r̂.
inline glm::vec3 spiky_grad(const glm::vec3& r, float dist, float h, float h6) {
    const float hr = h - dist;
    const float scale = -(45.0f / (kPi * h6)) * hr * hr;
    return (dist > 1e-9f) ? (r / dist) * scale : glm::vec3(0.0f);
}

// Laplaciano de viscosidade: ∇²W = 45/(π h^6) (h - r).
inline float visc_lap(float dist, float h, float h6) {
    if (dist >= h || dist <= 0.0f) return 0.0f;
    return (45.0f / (kPi * h6)) * (h - dist);
}

bool SphFluidConfig_validate(const SphFluidConfig& config,
                             std::string& errorOut) {
    if (config.maxParticles < 1 || config.maxParticles > (1u << 20)) {
        errorOut = "sph config: maxParticles must be in [1, 1<<20]";
        return false;
    }
    if (!(config.particleRadius > 0.0f) ||
        !std::isfinite(config.particleRadius)) {
        errorOut = "sph config: particleRadius must be finite and > 0";
        return false;
    }
    if (!(config.restDensity > 0.0f) || !std::isfinite(config.restDensity)) {
        errorOut = "sph config: restDensity must be finite and > 0";
        return false;
    }
    if (!(config.stiffness > 0.0f) || !std::isfinite(config.stiffness)) {
        errorOut = "sph config: stiffness must be finite and > 0";
        return false;
    }
    if (config.viscosity < 0.0f || !std::isfinite(config.viscosity)) {
        errorOut = "sph config: viscosity must be finite and >= 0";
        return false;
    }
    if (!std::isfinite(config.gravity)) {
        errorOut = "sph config: gravity must be finite";
        return false;
    }
    if (config.damping < 0.0f || config.damping >= 1.0f) {
        errorOut = "sph config: damping must be in [0, 1)";
        return false;
    }
    if (!std::isfinite(config.groundY)) {
        errorOut = "sph config: groundY must be finite";
        return false;
    }
    return true;
}

// Chave de célula da grade espacial: empacota (x, y, z) em int64 com bias —
// cada eixo vira um campo SEM SINAL de 21 bits (comportando coords
// negativas de forma deterministicamente decodificável). 3×21 = 63 bits
// cabem em int64 com shifts 21/42 sem sobreposição de campos (um campo de
// 22 bits transbordaria o bit 21 para o vizinho ao deslocar por 21). Faixa
// por eixo: [-2^20, 2^20-1] células — muito além do uso do solver.
constexpr std::int64_t kCellBias = 1ll << 20;
constexpr std::int64_t kCellMask = (1ll << 21) - 1;

inline std::int64_t cell_key(int x, int y, int z) {
    const std::int64_t fx = static_cast<std::int64_t>(x) + kCellBias;
    const std::int64_t fy = static_cast<std::int64_t>(y) + kCellBias;
    const std::int64_t fz = static_cast<std::int64_t>(z) + kCellBias;
    return (fx << 42) | (fy << 21) | fz;
}

inline void cell_key_decode(std::int64_t key, int& x, int& y, int& z) {
    x = static_cast<int>(((key >> 42) & kCellMask) - kCellBias);
    y = static_cast<int>(((key >> 21) & kCellMask) - kCellBias);
    z = static_cast<int>((key & kCellMask) - kCellBias);
}

struct Particle {
    glm::vec3 position{ 0.0f };
    glm::vec3 velocity{ 0.0f };
    glm::vec3 force{ 0.0f };
    float density{ 0.0f };
    float pressure{ 0.0f };
};

class SphFluidSimulation final : public ISPHFluidSimulation {
public:
    const SphFluidConfig& config() const noexcept override { return config_; }

    bool configure(const SphFluidConfig& config,
                   std::string& errorOut) override {
        if (!SphFluidConfig_validate(config, errorOut)) return false;
        config_ = config;
        particles_.clear();
        return true;
    }

    bool reset(const std::vector<glm::vec3>& positions,
               const std::vector<glm::vec3>& velocities,
               std::string& errorOut) override {
        if (positions.size() > config_.maxParticles) {
            errorOut = "sph: particle count exceeds maxParticles (" +
                       std::to_string(config_.maxParticles) + ")";
            return false;
        }
        if (!velocities.empty() && velocities.size() != positions.size()) {
            errorOut = "sph: velocities must match the position count";
            return false;
        }
        particles_.clear();
        particles_.reserve(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            Particle p;
            p.position = positions[i];
            p.velocity = velocities.empty() ? glm::vec3(0.0f) : velocities[i];
            particles_.push_back(p);
        }
        compute_particle_mass();  // massa disponível antes do 1º step
        return true;
    }

    void step(float dt) override {
        if (!(dt > 0.0f) || particles_.empty()) return;
        evaluate();
        integrate(dt);
    }

    std::size_t particle_count() const noexcept override {
        return particles_.size();
    }

    SphParticleState particle(std::size_t index) const noexcept override {
        SphParticleState s;
        if (index >= particles_.size()) return s;
        s.position = particles_[index].position;
        s.velocity = particles_[index].velocity;
        s.density = particles_[index].density;
        s.pressure = particles_[index].pressure;
        return s;
    }

    glm::vec3 particle_position(std::size_t index) const noexcept override {
        return index < particles_.size() ? particles_[index].position
                                         : glm::vec3(0.0f);
    }

    glm::vec3 particle_velocity(std::size_t index) const noexcept override {
        return index < particles_.size() ? particles_[index].velocity
                                         : glm::vec3(0.0f);
    }

    float particle_density(std::size_t index) const noexcept override {
        return index < particles_.size() ? particles_[index].density : 0.0f;
    }

    float particle_pressure(std::size_t index) const noexcept override {
        return index < particles_.size() ? particles_[index].pressure : 0.0f;
    }

    float total_mass() const noexcept override {
        return particle_mass_ * static_cast<float>(particles_.size());
    }

private:
    // Densidade (poly6), pressão de estado, forças de pressão (spiky),
    // viscosidade (laplaciano) e gravidade — tudo em ordem fixa.
    void evaluate() {
        const float r = config_.particleRadius;
        const float h = 4.0f * r;            // suporte do kernel
        const float h2 = h * h;
        const float h6 = h2 * h2 * h2;
        const float h9 = h6 * h2 * h;
        const float invCell = 1.0f / h;
        const float rho0 = config_.restDensity;
        const float k = config_.stiffness;
        const float mu = config_.viscosity;

        // Grade espacial: entradas (chave de célula, índice), ordenadas por
        // (chave, índice) — varredura determinística.
        std::vector<std::pair<std::int64_t, std::uint32_t>> grid;
        grid.reserve(particles_.size());
        for (std::uint32_t i = 0; i < particles_.size(); ++i) {
            const glm::vec3& p = particles_[i].position;
            const int cx = static_cast<int>(std::floor(p.x * invCell));
            const int cy = static_cast<int>(std::floor(p.y * invCell));
            const int cz = static_cast<int>(std::floor(p.z * invCell));
            grid.emplace_back(cell_key(cx, cy, cz), i);
        }
        std::sort(grid.begin(), grid.end());

        // Listas de vizinhos por partícula (índices ascendentes).
        std::vector<std::vector<std::uint32_t>> neighbors(particles_.size());
        for (std::size_t g = 0; g < grid.size();) {
            std::size_t runEnd = g;
            while (runEnd < grid.size() && grid[runEnd].first == grid[g].first)
                ++runEnd;
            for (std::size_t a = g; a < runEnd; ++a) {
                for (std::size_t b = g; b < runEnd; ++b) {
                    if (grid[a].second < grid[b].second)
                        neighbors[grid[a].second].push_back(grid[b].second);
                }
            }
            // Células vizinhas (26-vizinhança): decodifica as coordenadas da
            // célula atual e re-codifica cada vizinho explicitamente (sem
            // aritmética de chave, sem risco de carry entre campos). Ordem
            // lexicográfica fixa de offsets (dz, dy, dx).
            const std::int64_t key = grid[g].first;
            int cx = 0;
            int cy = 0;
            int cz = 0;
            cell_key_decode(key, cx, cy, cz);
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0 && dz == 0) continue;
                        // Só processa vizinhos com chave maior (par único).
                        const std::int64_t nk = cell_key(cx + dx, cy + dy, cz + dz);
                        if (nk <= key) continue;
                        const auto nIt = std::lower_bound(
                            grid.begin(), grid.end(),
                            std::make_pair(nk, std::numeric_limits<std::uint32_t>::min()));
                        if (nIt == grid.end() || nIt->first != nk) continue;
                        for (auto it = nIt; it != grid.end() && it->first == nk; ++it) {
                            for (std::size_t a = g; a < runEnd; ++a) {
                                if (grid[a].second < it->second)
                                    neighbors[grid[a].second].push_back(it->second);
                            }
                        }
                    }
                }
            }
            g = runEnd;
        }
        for (auto& list : neighbors) std::sort(list.begin(), list.end());

        // Densidade e pressão (ordem fixa de partículas). O termo próprio usa
        // W(0) = 315/(64π h³) (o kernel poly6 devolve 0 em r2 <= 0).
        compute_particle_mass();
        const float wZero = 315.0f / (64.0f * kPi * h2 * h);
        for (std::size_t i = 0; i < particles_.size(); ++i) {
            float rho = particle_mass_ * wZero;
            for (const std::uint32_t j : neighbors[i]) {
                const glm::vec3 dr = particles_[i].position -
                                     particles_[j].position;
                const float r2 = glm::dot(dr, dr);
                rho += particle_mass_ * poly6(r2, h, h9);
            }
            particles_[i].density = rho;
            particles_[i].pressure = k * (rho - rho0);
        }

        // Forças (pares únicos, ordem de vizinhança já fixa).
        for (auto& p : particles_) p.force = glm::vec3(0.0f);
        for (std::size_t i = 0; i < particles_.size(); ++i) {
            const float rho_i = particles_[i].density;
            const float p_i = particles_[i].pressure;
            for (const std::uint32_t j : neighbors[i]) {
                if (j <= i) continue;
                const glm::vec3 r = particles_[i].position -
                                    particles_[j].position;
                const float dist = glm::length(r);
                if (dist >= h || dist <= 1e-9f) continue;
                const float rho_j = particles_[j].density;
                const float p_j = particles_[j].pressure;
                const glm::vec3 grad = spiky_grad(r, dist, h, h6);
                // Pressão (simétrico, conserva momento).
                const float coef = particle_mass_ * (p_i + p_j) /
                                   (2.0f * rho_i * rho_j);
                particles_[i].force -= coef * grad;
                particles_[j].force += coef * grad;
                // Viscosidade.
                const float lap = visc_lap(dist, h, h6);
                const float vcoef = particle_mass_ * mu * lap / rho_j;
                const glm::vec3 dv = particles_[j].velocity -
                                     particles_[i].velocity;
                particles_[i].force += vcoef * dv;
                particles_[j].force -= vcoef * dv;
            }
            particles_[i].force.y += particle_mass_ * config_.gravity;
        }
    }

    // Massa calibrada para a densidade de repouso: um par no espaçamento 2r
    // (mínimo de partículas que forma um fluido) tem densidade == ρ0.
    void compute_particle_mass() {
        const float r = config_.particleRadius;
        const float h = 4.0f * r;
        const float h2 = h * h;
        const float h6 = h2 * h2 * h2;
        const float h9 = h6 * h2 * h;
        const float wZero = 315.0f / (64.0f * kPi * h * h2);  // W(0) = 315/(64π h³)
        const float wRest = poly6((2.0f * r) * (2.0f * r), h, h9);
        particle_mass_ = config_.restDensity / (wZero + wRest);
    }

    void integrate(float dt) {
        const float r = config_.particleRadius;
        const float damp = 1.0f - config_.damping;
        for (auto& p : particles_) {
            const glm::vec3 a = p.force / particle_mass_;
            p.velocity = (p.velocity + a * dt) * damp;
            p.position += p.velocity * dt;
            if (p.position.y < config_.groundY + r) {
                p.position.y = config_.groundY + r;
                p.velocity.y = 0.0f;  // restituição 0
            }
        }
    }

    SphFluidConfig config_;
    std::vector<Particle> particles_;
    float particle_mass_{ 0.0f };
};

}  // namespace

bool SphFluidConfig::valid(std::string& errorOut) const {
    return SphFluidConfig_validate(*this, errorOut);
}

bool SphFluidConfig::load_from_json(const std::string& json,
                                    std::string& errorOut) {
    // Parser JSON mínimo e determinístico (números e strings), sem depender
    // de nenhuma lib — mesmo formato dos demais contratos do SDK.
    SphFluidConfig candidate = *this;
    bool any = false;
    const std::string keys[] = { "maxParticles", "particleRadius",
                                 "restDensity",  "stiffness",
                                 "viscosity",    "gravity",
                                 "damping",      "groundY" };
    float* floats[] = { nullptr,           &candidate.particleRadius,
                        &candidate.restDensity, &candidate.stiffness,
                        &candidate.viscosity,   &candidate.gravity,
                        &candidate.damping,     &candidate.groundY };
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
        bool matched = false;
        for (std::size_t i = 0; i < 8; ++i) {
            if (key == keys[i]) {
                if (i == 0) {
                    candidate.maxParticles = static_cast<std::uint32_t>(
                        std::strtoul(value.c_str(), nullptr, 10));
                } else {
                    *floats[i] = std::strtof(value.c_str(), nullptr);
                }
                matched = true;
                any = true;
                break;
            }
        }
        (void)matched;
        pos = vEnd == std::string::npos ? json.size() : vEnd + 1;
    }
    if (!any) {
        errorOut = "sph config: no recognized keys";
        return false;
    }
    if (!SphFluidConfig_validate(candidate, errorOut)) return false;
    *this = candidate;
    return true;
}

std::string SphFluidConfig::to_json() const {
    std::string out = "{";
    out += "\"maxParticles\":" + std::to_string(maxParticles) + ",";
    out += "\"particleRadius\":" + std::to_string(particleRadius) + ",";
    out += "\"restDensity\":" + std::to_string(restDensity) + ",";
    out += "\"stiffness\":" + std::to_string(stiffness) + ",";
    out += "\"viscosity\":" + std::to_string(viscosity) + ",";
    out += "\"gravity\":" + std::to_string(gravity) + ",";
    out += "\"damping\":" + std::to_string(damping) + ",";
    out += "\"groundY\":" + std::to_string(groundY);
    out += "}";
    return out;
}

std::unique_ptr<ISPHFluidSimulation> create_sph_fluid_simulation(
    std::string& errorOut) {
    auto impl = std::make_unique<SphFluidSimulation>();
    if (!impl) {
        errorOut = "sph: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<ISPHFluidSimulation> create_sph_fluid_simulation_json(
    const std::string& jsonText, std::string& errorOut) {
    SphFluidConfig config;
    if (!config.load_from_json(jsonText, errorOut)) return nullptr;
    auto impl = std::make_unique<SphFluidSimulation>();
    if (!impl->configure(config, errorOut)) return nullptr;
    return impl;
}

}  // namespace simulation
}  // namespace engine
