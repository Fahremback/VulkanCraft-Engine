// MultibodyDynamics.cpp — the only TU implementing IMultibodyDynamics.
//
// Dinâmica de corpos articulados em coordenadas generalizadas, do zero:
//   - FK: o_k = o_{k-1} + R_{k-1}·(a_k·q_k [prismatic] + offset_k),
//         R_k = R_{k-1}·Rot(a_k, q_k) [revolute];
//   - Jacobianos J_k (linear) e Jω_k (rotacional) por coluna de junta;
//   - M(q) = Σ m_k·J_kᵀJ_k + Jω_kᵀ·I_k·Jω_k; g(q) = Σ m_k·J_kᵀ·gravidade;
//   - q̈ = M⁻¹(τ − g) resolvido por eliminação de Gauss com pivotamento
//     parcial (determinística, sem threading);
//   - integração semi-implícita de Euler, damping e limites de junta.

#include "engine/physics/IMultibodyDynamics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

namespace engine {
namespace physics {
namespace {

bool MultibodyConfig_validate(const MultibodyConfig& config,
                              std::string& errorOut) {
    if (config.maxLinks < 1 || config.maxLinks > 64) {
        errorOut = "multibody config: maxLinks must be in [1, 64]";
        return false;
    }
    if (config.solverIterations < 1 || config.solverIterations > 64) {
        errorOut = "multibody config: solverIterations must be in [1, 64]";
        return false;
    }
    if (config.damping < 0.0f || config.damping >= 1.0f) {
        errorOut = "multibody config: damping must be in [0, 1)";
        return false;
    }
    if (glm::length(config.gravity) > 1000.0f) {
        errorOut = "multibody config: gravity magnitude exceeds 1000";
        return false;
    }
    return true;
}

struct ChainLink {
    JointKind joint{ JointKind::Revolute };
    glm::vec3 axis{ 0.0f, 0.0f, 1.0f };   // no frame do pai (normalizado)
    glm::vec3 offset{ 0.0f, 0.0f, 0.0f }; // COM no frame do pai
    float mass{ 1.0f };
    glm::vec3 inertia{ 1.0f, 1.0f, 1.0f };
    float jointMin{ -3.14159265f };
    float jointMax{ 3.14159265f };
    float q{ 0.0f };
    float qdot{ 0.0f };
};

struct Chain {
    MultibodyHandle handle{ InvalidMultibody };
    std::vector<ChainLink> links;
};

// FK result for one chain: world frames (origin = COM) + joint pivots.
struct ChainFk {
    std::vector<glm::vec3> origins;       // o_k (COM no mundo)
    std::vector<glm::mat3> orientations;  // R_k
    std::vector<glm::vec3> pivots;        // pivô da junta k (o_{k-1})
    std::vector<glm::vec3> axes;          // a_k no mundo
};

ChainFk forward_kinematics(const Chain& chain) {
    ChainFk fk;
    const std::size_t n = chain.links.size();
    fk.origins.resize(n);
    fk.orientations.resize(n);
    fk.pivots.resize(n);
    fk.axes.resize(n);
    glm::vec3 prevOrigin(0.0f);
    glm::mat3 prevRot = glm::mat3(1.0f);
    for (std::size_t k = 0; k < n; ++k) {
        const ChainLink& link = chain.links[k];
        const glm::vec3 axisW = prevRot * link.axis;
        fk.pivots[k] = prevOrigin;
        fk.axes[k] = axisW;
        if (link.joint == JointKind::Revolute) {
            const float c = std::cos(link.q);
            const float s = std::sin(link.q);
            // Rotação de Rodrigues em torno de axisW (frame do pai).
            const glm::vec3& a = link.axis;
            const glm::mat3 rot(
                glm::vec3(c + a.x * a.x * (1 - c),
                          a.y * a.x * (1 - c) + a.z * s,
                          a.z * a.x * (1 - c) - a.y * s),
                glm::vec3(a.x * a.y * (1 - c) - a.z * s,
                          c + a.y * a.y * (1 - c),
                          a.z * a.y * (1 - c) + a.x * s),
                glm::vec3(a.x * a.z * (1 - c) + a.y * s,
                          a.y * a.z * (1 - c) - a.x * s,
                          c + a.z * a.z * (1 - c)));
            fk.orientations[k] = prevRot * rot;
            // O COM está fixo no frame do LINK: rotaciona com a junta.
            fk.origins[k] = prevOrigin + fk.orientations[k] * link.offset;
        } else {  // Prismatic
            fk.orientations[k] = prevRot;
            fk.origins[k] =
                prevOrigin + prevRot * (link.offset + link.axis * link.q);
        }
        prevOrigin = fk.origins[k];
        prevRot = fk.orientations[k];
    }
    return fk;
}

class MultibodyDynamics final : public IMultibodyDynamics {
private:
    using VecN = std::vector<float>;
    using MatN = std::vector<VecN>;  // linha-major, M[r][c]

public:
    const MultibodyConfig& config() const noexcept override {
        return config_;
    }

    bool configure(const MultibodyConfig& config,
                   std::string& errorOut) override {
        if (!MultibodyConfig_validate(config, errorOut)) return false;
        config_ = config;
        chains_.clear();
        return true;
    }

    MultibodyHandle create_chain(const std::vector<MultibodyLinkDesc>& links,
                                 std::string& errorOut) override {
        if (links.empty()) {
            errorOut = "multibody: empty chain refused";
            return InvalidMultibody;
        }
        if (links.size() > config_.maxLinks) {
            errorOut = "multibody: chain exceeds maxLinks (" +
                       std::to_string(config_.maxLinks) + ")";
            return InvalidMultibody;
        }
        Chain chain;
        chain.links.reserve(links.size());
        for (const auto& desc : links) {
            if (!(desc.mass > 0.0f) || !std::isfinite(desc.mass)) {
                errorOut = "multibody: link mass must be finite and > 0";
                return InvalidMultibody;
            }
            if (!(desc.inertia.x > 0.0f && desc.inertia.y > 0.0f &&
                  desc.inertia.z > 0.0f)) {
                errorOut = "multibody: link inertia must be > 0 on all axes";
                return InvalidMultibody;
            }
            if (glm::length(desc.jointAxis) < 1e-6f) {
                errorOut = "multibody: joint axis must be non-zero";
                return InvalidMultibody;
            }
            if (!(desc.jointMin <= desc.jointMax)) {
                errorOut = "multibody: joint limits inverted (min > max)";
                return InvalidMultibody;
            }
            ChainLink link;
            link.joint = desc.joint;
            link.axis = glm::normalize(desc.jointAxis);
            link.offset = desc.offset;
            link.mass = desc.mass;
            link.inertia = desc.inertia;
            link.jointMin = desc.jointMin;
            link.jointMax = desc.jointMax;
            link.q = glm::clamp(desc.jointAngle, desc.jointMin, desc.jointMax);
            chain.links.push_back(link);
        }
        chain.handle = ++nextHandle_;
        chains_[chain.handle] = std::move(chain);
        return chain.handle;
    }

    bool destroy_chain(MultibodyHandle chain) override {
        return chains_.erase(chain) > 0;
    }

    std::size_t chain_count() const noexcept override { return chains_.size(); }

    void step(float dt) override {
        if (!(dt > 0.0f)) return;
        // Ordem fixa de cadeias e de juntas. chains_ é um std::map ordenado
        // por handle — e como os handles são atribuídos monotonicamente
        // (++nextHandle_), a ordem de iteração É a ordem de inserção.
        // (unordered_map NÃO garante ordem de inserção; a alegação de
        // determinismo dependia disso.)
        const float subDt = dt / static_cast<float>(config_.solverIterations);
        for (int pass = 0; pass < config_.solverIterations; ++pass) {
            const bool last = pass == config_.solverIterations - 1;
            for (auto& kv : chains_) step_chain(kv.second, subDt, last);
        }
    }

    std::size_t link_count(MultibodyHandle chain) const noexcept override {
        const auto found = chains_.find(chain);
        return found == chains_.end() ? 0 : found->second.links.size();
    }

    MultibodyLinkState link_state(MultibodyHandle chain,
                                  std::size_t linkIndex) const noexcept override {
        MultibodyLinkState state;
        const auto found = chains_.find(chain);
        if (found == chains_.end() || linkIndex >= found->second.links.size())
            return state;
        const Chain& c = found->second;
        const ChainFk fk = forward_kinematics(c);
        const std::size_t n = c.links.size();
        // Velocidade do COM no mundo: Σ_j J_kj · q̇_j.
        glm::vec3 velocity(0.0f);
        for (std::size_t j = 0; j <= linkIndex; ++j) {
            const glm::vec3 jac = jacobian_column(c, fk, linkIndex, j);
            velocity += jac * c.links[j].qdot;
        }
        state.position = fk.origins[linkIndex];
        state.velocity = velocity;
        state.orientation = glm::quat_cast(fk.orientations[linkIndex]);
        state.jointAngle = c.links[linkIndex].q;
        state.jointVelocity = c.links[linkIndex].qdot;
        (void)n;
        return state;
    }

private:
    // Coluna j do Jacobiano linear da link k (no mundo).
    static glm::vec3 jacobian_column(const Chain& chain, const ChainFk& fk,
                                     std::size_t k, std::size_t j) {
        if (j > k) return glm::vec3(0.0f);
        const ChainLink& link = chain.links[j];
        if (link.joint == JointKind::Prismatic) return fk.axes[j];
        return glm::cross(fk.axes[j], fk.origins[k] - fk.pivots[j]);
    }

    // Coluna j do Jacobiano rotacional da link k.
    static glm::vec3 angular_jacobian_column(const Chain& chain,
                                             const ChainFk& fk,
                                             std::size_t k, std::size_t j) {
        if (j > k) return glm::vec3(0.0f);
        return chain.links[j].joint == JointKind::Revolute ? fk.axes[j]
                                                           : glm::vec3(0.0f);
    }

    void step_chain(Chain& chain, float dt, bool finalizePass) {
        const std::size_t n = chain.links.size();
        const ChainFk fk = forward_kinematics(chain);

        // M(q) e g(q) (matriz densa n x n — n <= maxLinks <= 64).
        MatN M(n, VecN(n, 0.0f));
        VecN g(n, 0.0f);
        for (std::size_t k = 0; k < n; ++k) {
            const ChainLink& link = chain.links[k];
            for (std::size_t j = 0; j <= k; ++j) {
                const glm::vec3 jac = jacobian_column(chain, fk, k, j);
                const glm::vec3 jaw = angular_jacobian_column(chain, fk, k, j);
                for (std::size_t m = 0; m <= k; ++m) {
                    const glm::vec3 jacM = jacobian_column(chain, fk, k, m);
                    const glm::vec3 jawM =
                        angular_jacobian_column(chain, fk, k, m);
                    float mij = link.mass * glm::dot(jac, jacM);
                    mij += jaw.x * link.inertia.x * jawM.x +
                           jaw.y * link.inertia.y * jawM.y +
                           jaw.z * link.inertia.z * jawM.z;
                    M[j][m] += mij;
                }
                g[j] += link.mass * glm::dot(jac, config_.gravity);
            }
        }

        // q̈ = M⁻¹(τ + g) com τ = 0, onde g é a força generalizada de
        // gravidade Σ m_k·J_kᵀ·g_vec (já com o sinal correto: para o pêndulo
        // em q=0, g = -m·g·L e q̈ = -g/M < 0 — desce). Nesta versão a API
        // integra gravidade, damping e limites; torques externos entram em
        // evolução futura.
        const VecN qdd = solve_linear(M, g);

        // Integração semi-implícita. solverIterations divide a chamada em
        // passes internos (cada um re-avalia M/g nas posições atualizadas —
        // antes o parâmetro era validado mas não participava da resolução).
        // Damping e limites são aplicados uma única vez por step() (na última
        // passagem), preservando a taxa efetiva de amortecimento.
        for (std::size_t j = 0; j < n; ++j) {
            ChainLink& link = chain.links[j];
            link.qdot += qdd[j] * dt;
            link.q += link.qdot * dt;
            if (finalizePass) {
                link.qdot *= (1.0f - config_.damping);
                if (link.q < link.jointMin || link.q > link.jointMax) {
                    link.q = glm::clamp(link.q, link.jointMin, link.jointMax);
                    link.qdot = 0.0f;
                }
            }
        }
    }

    // Eliminação de Gauss com pivotamento parcial (determinística).
    static VecN solve_linear(MatN M, VecN b) {
        const std::size_t n = b.size();
        for (std::size_t col = 0; col < n; ++col) {
            std::size_t pivot = col;
            float maxAbs = std::fabs(M[col][col]);
            for (std::size_t r = col + 1; r < n; ++r) {
                const float a = std::fabs(M[r][col]);
                if (a > maxAbs) {
                    maxAbs = a;
                    pivot = r;
                }
            }
            if (maxAbs < 1e-12f) continue;  // singular — deixa zero
            if (pivot != col) {
                for (std::size_t c = 0; c < n; ++c)
                    std::swap(M[col][c], M[pivot][c]);
                std::swap(b[col], b[pivot]);
            }
            const float inv = 1.0f / M[col][col];
            for (std::size_t c = col; c < n; ++c) M[col][c] *= inv;
            b[col] *= inv;
            for (std::size_t r = 0; r < n; ++r) {
                if (r == col) continue;
                const float f = M[r][col];
                if (f == 0.0f) continue;
                for (std::size_t c = col; c < n; ++c)
                    M[r][c] -= f * M[col][c];
                b[r] -= f * b[col];
            }
        }
        return b;
    }

    MultibodyConfig config_;
    std::map<MultibodyHandle, Chain> chains_;
    MultibodyHandle nextHandle_{ InvalidMultibody };
};

}  // namespace

bool MultibodyConfig::valid(std::string& errorOut) const {
    return MultibodyConfig_validate(*this, errorOut);
}

bool MultibodyConfig::load_from_json(const std::string& json,
                                     std::string& errorOut) {
    MultibodyConfig candidate = *this;
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
        if (key == "maxLinks") {
            candidate.maxLinks = static_cast<std::size_t>(
                std::strtoul(value.c_str(), nullptr, 10));
            any = true;
        } else if (key == "solverIterations") {
            candidate.solverIterations = static_cast<int>(
                std::strtol(value.c_str(), nullptr, 10));
            any = true;
        } else if (key == "damping") {
            candidate.damping = std::strtof(value.c_str(), nullptr);
            any = true;
        }
        pos = vEnd == std::string::npos ? json.size() : vEnd + 1;
    }
    if (!any) {
        errorOut = "multibody config: no recognized keys";
        return false;
    }
    if (!MultibodyConfig_validate(candidate, errorOut)) return false;
    *this = candidate;
    return true;
}

std::string MultibodyConfig::to_json() const {
    std::string out = "{";
    out += "\"maxLinks\":" + std::to_string(maxLinks) + ",";
    out += "\"solverIterations\":" + std::to_string(solverIterations) + ",";
    out += "\"damping\":" + std::to_string(damping);
    out += "}";
    return out;
}

std::unique_ptr<IMultibodyDynamics> create_multibody_dynamics(
    std::string& errorOut) {
    auto impl = std::make_unique<MultibodyDynamics>();
    if (!impl) {
        errorOut = "multibody: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IMultibodyDynamics> create_multibody_dynamics_json(
    const std::string& jsonText, std::string& errorOut) {
    MultibodyConfig config;
    if (!config.load_from_json(jsonText, errorOut)) return nullptr;
    auto impl = std::make_unique<MultibodyDynamics>();
    if (!impl->configure(config, errorOut)) return nullptr;
    return impl;
}

}  // namespace physics
}  // namespace engine
