// FemfxDeformable.cpp — FEM truss provider behind IDeformableProvider.
//
// A contraparte headless/determinística do FEMFX (AMD): FEM LINEAR de treliça
// (elementos de barra) sobre a mesma malha node/edge do contrato. Cada aresta
// é um elemento finito de mola com matriz de rigidez concentrada:
//   f = k_e · (|x_b − x_a| − L0) · dir,  k_e = stiffness · K_BASE
// Forças internas montadas por elemento em ordem fixa, integração
// semi-implícita de Euler com gravidade/forças externas, damping, piso e
// restituição. Determinístico (sem RNG, ordem fixa de nós/arestas).
//
// A validação da config replica as regras do XpbdDeformable (mesmo
// DeformableConfig) — este TU não compartilha o helper interno daquele.

#include "engine/deformable/IDeformableProvider.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Engine::Deformable {
namespace {

constexpr float kBaseStiffness = 10000.0f;  // N/m por unidade de stiffness (0,1]

bool FemfxConfig_validate(const DeformableConfig& config,
                          std::string& errorOut) {
    if (config.maxNodes < 1 || config.maxNodes > 1000000) {
        errorOut = "femfx config: maxNodes must be in [1, 1000000]";
        return false;
    }
    if (config.substeps < 1 || config.substeps > 16) {
        errorOut = "femfx config: substeps must be in [1, 16]";
        return false;
    }
    if (config.solverIterations < 1 || config.solverIterations > 64) {
        errorOut = "femfx config: solverIterations must be in [1, 64]";
        return false;
    }
    if (config.stiffness <= 0.0f || config.stiffness > 1.0f) {
        errorOut = "femfx config: stiffness must be in (0, 1]";
        return false;
    }
    if (config.damping < 0.0f || config.damping >= 1.0f) {
        errorOut = "femfx config: damping must be in [0, 1)";
        return false;
    }
    if (glm::length(config.gravity) > 1000.0f) {
        errorOut = "femfx config: gravity magnitude exceeds 1000";
        return false;
    }
    if (config.bounce < 0.0f || config.bounce > 1.0f) {
        errorOut = "femfx config: bounce must be in [0, 1]";
        return false;
    }
    return true;
}

struct FemNode {
    glm::vec3 position{ 0.0f };  // corrente
    glm::vec3 rest{ 0.0f };      // posição de repouso (malha original)
    glm::vec3 velocity{ 0.0f };
    glm::vec3 force{ 0.0f };     // forças externas acumuladas
    float inverseMass{ 1.0f };   // 0 = ancorado
};

struct FemEdge {
    std::uint32_t a{ 0 };
    std::uint32_t b{ 0 };
    float restLength{ 1.0f };
    float k{ 1.0f };             // rigidez do elemento (N/m)
};

struct FemBody {
    DeformableBodyHandle handle{ InvalidDeformableBody };
    std::vector<FemNode> nodes;
    std::vector<FemEdge> edges;
};

class FemfxProvider final : public IDeformableProvider {
public:
    explicit FemfxProvider(const DeformableConfig& config) : config_(config) {}

    DeformableProviderKind kind() const noexcept override {
        return DeformableProviderKind::Femfx;
    }

    const DeformableConfig& config() const noexcept override {
        return config_;
    }

    DeformableBodyHandle create_body(const DeformableMeshDesc& desc,
                                     std::string& errorOut) override {
        if (desc.nodes.empty()) {
            errorOut = "femfx: empty mesh refused";
            return InvalidDeformableBody;
        }
        if (desc.nodes.size() > config_.maxNodes) {
            errorOut = "femfx: mesh exceeds maxNodes (" +
                       std::to_string(config_.maxNodes) + ")";
            return InvalidDeformableBody;
        }
        if (desc.edges.empty()) {
            errorOut = "femfx: mesh needs at least one edge (truss element)";
            return InvalidDeformableBody;
        }
        if (!desc.fixed.empty() && desc.fixed.size() != desc.nodes.size()) {
            errorOut = "femfx: fixed flags must match the node count";
            return InvalidDeformableBody;
        }
        if (!desc.stiffness.empty() && desc.stiffness.size() != desc.edges.size()) {
            errorOut = "femfx: per-edge stiffness must match the edge count";
            return InvalidDeformableBody;
        }
        for (const auto& edge : desc.edges) {
            if (edge.first >= desc.nodes.size() || edge.second >= desc.nodes.size()) {
                errorOut = "femfx: edge references an invalid node index";
                return InvalidDeformableBody;
            }
        }
        for (const float k : desc.stiffness) {
            if (!(k > 0.0f) || k > 1.0f) {
                errorOut = "femfx: per-edge stiffness must be in (0, 1]";
                return InvalidDeformableBody;
            }
        }

        FemBody body;
        body.nodes.resize(desc.nodes.size());
        for (std::size_t i = 0; i < desc.nodes.size(); ++i) {
            body.nodes[i].position = desc.nodes[i];
            body.nodes[i].rest = desc.nodes[i];
            if (!desc.fixed.empty() && desc.fixed[i]) body.nodes[i].inverseMass = 0.0f;
        }
        body.edges.reserve(desc.edges.size());
        for (std::size_t i = 0; i < desc.edges.size(); ++i) {
            FemEdge e;
            e.a = desc.edges[i].first;
            e.b = desc.edges[i].second;
            e.restLength = glm::distance(body.nodes[e.a].position,
                                         body.nodes[e.b].position);
            if (e.restLength < 1e-6f) {
                errorOut = "femfx: zero-length truss element refused";
                return InvalidDeformableBody;
            }
            e.k = (desc.stiffness.empty() ? config_.stiffness
                                          : desc.stiffness[i]) *
                  kBaseStiffness;
            body.edges.push_back(e);
        }
        body.handle = ++nextHandle_;
        bodies_[body.handle] = std::move(body);
        return body.handle;
    }

    bool destroy_body(DeformableBodyHandle body) override {
        return bodies_.erase(body) > 0;
    }

    std::size_t body_count() const noexcept override { return bodies_.size(); }

    void apply_force(DeformableBodyHandle body, std::uint32_t node,
                     const glm::vec3& force) override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end() || node >= found->second.nodes.size()) return;
        found->second.nodes[node].force += force;
    }

    void set_edge_stiffness(DeformableBodyHandle body, std::uint32_t edgeIndex,
                            float stiffness) override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end() || edgeIndex >= found->second.edges.size())
            return;
        FemEdge& edge = found->second.edges[edgeIndex];
        // stiffness == 0 = elemento desativado (parte separada); (0, 1] reescala.
        edge.k = glm::clamp(stiffness, 0.0f, 1.0f) * kBaseStiffness;
    }

    void step(float dt) override {
        if (!(dt > 0.0f)) return;
        const float subDt = dt / static_cast<float>(config_.substeps);
        for (int s = 0; s < config_.substeps; ++s) {
            for (auto& kv : bodies_) step_body(kv.second, subDt);
        }
    }

    std::size_t node_count(DeformableBodyHandle body) const noexcept override {
        const auto found = bodies_.find(body);
        return found == bodies_.end() ? 0 : found->second.nodes.size();
    }

    glm::vec3 node_position(DeformableBodyHandle body,
                            std::uint32_t node) const noexcept override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end() || node >= found->second.nodes.size())
            return glm::vec3(0.0f);
        return found->second.nodes[node].position;
    }

    glm::vec3 node_velocity(DeformableBodyHandle body,
                            std::uint32_t node) const noexcept override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end() || node >= found->second.nodes.size())
            return glm::vec3(0.0f);
        return found->second.nodes[node].velocity;
    }

    float constraint_error(DeformableBodyHandle body) const noexcept override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end() || found->second.edges.empty()) return 0.0f;
        const FemBody& b = found->second;
        double sum = 0.0;
        for (const FemEdge& e : b.edges) {
            const float len = glm::distance(b.nodes[e.a].position,
                                            b.nodes[e.b].position);
            sum += std::fabs(len - e.restLength);
        }
        return static_cast<float>(sum / static_cast<double>(b.edges.size()));
    }

private:
    void step_body(FemBody& body, float subDt) {
        // 1) Monta forças internas (ordem fixa de arestas) + gravidade.
        std::vector<glm::vec3> f(body.nodes.size(), glm::vec3(0.0f));
        for (const FemEdge& e : body.edges) {
            if (e.k <= 0.0f) continue;  // elemento desativado
            const glm::vec3 r = body.nodes[e.b].position -
                                body.nodes[e.a].position;
            const float len = glm::length(r);
            if (len < 1e-9f) continue;
            const glm::vec3 dir = r / len;  // de a para b
            const float axial = e.k * (len - e.restLength);
            const glm::vec3 force = dir * axial;
            // Elemento esticado (axial > 0): a é puxado para b (+dir) e b é
            // puxado para a (−dir); comprimido, os sinais invertem — simétrico.
            f[e.a] += force;
            f[e.b] -= force;
        }
        for (std::size_t i = 0; i < body.nodes.size(); ++i) {
            f[i] += body.nodes[i].force;  // externas acumuladas
            if (body.nodes[i].inverseMass > 0.0f)
                f[i] += config_.gravity;  // massa unitária
        }

        // 2) Integração semi-implícita (nós em ordem).
        const float damp = 1.0f - config_.damping;
        for (std::size_t i = 0; i < body.nodes.size(); ++i) {
            FemNode& n = body.nodes[i];
            if (n.inverseMass <= 0.0f) continue;  // ancorado
            n.velocity = (n.velocity + f[i] * n.inverseMass * subDt) * damp;
            n.position += n.velocity * subDt;
            if (config_.groundCollision &&
                n.position.y < config_.groundY) {
                n.position.y = config_.groundY;
                if (n.velocity.y < 0.0f)
                    n.velocity.y = -n.velocity.y * config_.bounce;
            }
        }

        // 3) Limpa forças externas acumuladas (próximo step()).
        for (auto& n : body.nodes) n.force = glm::vec3(0.0f);
    }

    DeformableConfig config_;
    std::unordered_map<DeformableBodyHandle, FemBody> bodies_;
    DeformableBodyHandle nextHandle_{ InvalidDeformableBody };
};

}  // namespace

std::unique_ptr<IDeformableProvider> create_femfx_provider(
    const DeformableConfig& config, std::string& errorOut) {
    std::string validateError;
    if (!FemfxConfig_validate(config, validateError)) {
        errorOut = validateError;
        return nullptr;
    }
    return std::make_unique<FemfxProvider>(config);
}

}  // namespace Engine::Deformable
