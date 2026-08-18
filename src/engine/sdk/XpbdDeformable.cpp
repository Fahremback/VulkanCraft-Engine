// XpbdDeformable.cpp
//
// The XPBD plugin behind IDeformableProvider (FALTANTES §16 item 3): a
// self-contained position-based-dynamics solver (same family as the XPBD
// paper — Müller, Macklin, Chentanez et al. "XPBD: Position-Based Simulation
// of Compliant Constrained Dynamics") implemented natively, deterministic and
// headless. The ONLY TU that crosses into the deformable solver; the public
// contract is everything the caller sees.
//
//   integrate   — per substep: velocity += dt*(gravity + force/mass),
//                 predicted position x = p + dt*v, damping applied to v.
//   solve       — solverIterations XPBD passes over the distance constraints:
//                 each edge's compliance-scaled Lagrange multiplier is
//                 updated and the endpoints projected by inverse mass. Fixed
//                 nodes (inverse mass 0) never move.
//   ground      — nodes below groundY are clamped back with bounce restitution.
//   finalize    — v = (x - p)/dt; p = x; accumulated forces cleared.
//
// Deterministic: nodes and edges iterate in creation order, no randomness,
// fixed epsilon for degenerate-edge fallback. Identical inputs -> bit-identical
// states (item 5 formalizes the guarantee).

#include "engine/deformable/IDeformableProvider.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <limits>
#include <unordered_map>

namespace Engine::Deformable {

namespace {

constexpr float kEpsilon = 1.0e-6f;
// Compliance of a separated edge (stiffness 0 via set_edge_stiffness): large
// enough that the XPBD correction is negligible — the constraint is inert.
constexpr float kSeparatedAlpha = 1.0e9f;

bool DeformableConfig_validate(const DeformableConfig& config,
                               std::string& errorOut) {
    if (config.maxNodes < 1 || config.maxNodes > 1000000) {
        errorOut = "deformable config: maxNodes must be in [1, 1000000]";
        return false;
    }
    if (config.substeps < 1 || config.substeps > 16) {
        errorOut = "deformable config: substeps must be in [1, 16]";
        return false;
    }
    if (config.solverIterations < 1 || config.solverIterations > 64) {
        errorOut = "deformable config: solverIterations must be in [1, 64]";
        return false;
    }
    if (config.stiffness <= 0.0f || config.stiffness > 1.0f) {
        errorOut = "deformable config: stiffness must be in (0, 1]";
        return false;
    }
    if (config.damping < 0.0f || config.damping >= 1.0f) {
        errorOut = "deformable config: damping must be in [0, 1)";
        return false;
    }
    const float g = glm::length(config.gravity);
    if (g > 1000.0f) {
        errorOut = "deformable config: gravity magnitude exceeds 1000";
        return false;
    }
    if (config.bounce < 0.0f || config.bounce > 1.0f) {
        errorOut = "deformable config: bounce must be in [0, 1]";
        return false;
    }
    return true;
}

struct Node {
    glm::vec3 position{ 0.0f };       // current (last solved) position
    glm::vec3 predicted{ 0.0f };      // XPBD predicted position (being solved)
    glm::vec3 velocity{ 0.0f };
    glm::vec3 force{ 0.0f };          // accumulated until the next step()
    float inverseMass{ 1.0f };        // 0 = fixed/anchored
};

struct Edge {
    std::uint32_t a{ 0 };
    std::uint32_t b{ 0 };
    float restLength{ 1.0f };
    float alpha{ 0.0f };              // compliance (1-k)/k; per-edge override
    float lambda{ 0.0f };             // XPBD Lagrange multiplier (persists)
};

struct Body {
    DeformableBodyHandle handle{ InvalidDeformableBody };
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

}  // namespace

bool DeformableConfig::load_from_json(const std::string& json,
                                      std::string& errorOut) {
    engine::sdk::JsonValue document;
    if (!engine::sdk::json_parse(json, document, errorOut)) return false;
    if (!document.is_object()) {
        errorOut = "deformable config must be a JSON object";
        return false;
    }
    // C4700 guard (findings #72/#74): defaults read the MEMBERS with this->.
    const double maxNodes =
        engine::sdk::json_number(document, "maxNodes", static_cast<double>(this->maxNodes));
    const double substeps =
        engine::sdk::json_number(document, "substeps", static_cast<double>(this->substeps));
    const double solverIterations = engine::sdk::json_number(
        document, "solverIterations", static_cast<double>(this->solverIterations));
    const double stiffness =
        engine::sdk::json_number(document, "stiffness", static_cast<double>(this->stiffness));
    const double damping =
        engine::sdk::json_number(document, "damping", static_cast<double>(this->damping));
    const double groundY =
        engine::sdk::json_number(document, "groundY", static_cast<double>(this->groundY));
    const double bounce =
        engine::sdk::json_number(document, "bounce", static_cast<double>(this->bounce));

    DeformableConfig candidate;
    candidate.maxNodes = static_cast<std::size_t>(maxNodes);
    candidate.substeps = static_cast<int>(substeps);
    candidate.solverIterations = static_cast<int>(solverIterations);
    candidate.stiffness = static_cast<float>(stiffness);
    candidate.damping = static_cast<float>(damping);
    candidate.groundY = static_cast<float>(groundY);
    candidate.bounce = static_cast<float>(bounce);
    candidate.gravity.x = static_cast<float>(
        engine::sdk::json_number(document, "gravityX", static_cast<double>(gravity.x)));
    candidate.gravity.y = static_cast<float>(
        engine::sdk::json_number(document, "gravityY", static_cast<double>(gravity.y)));
    candidate.gravity.z = static_cast<float>(
        engine::sdk::json_number(document, "gravityZ", static_cast<double>(gravity.z)));
    candidate.groundCollision =
        engine::sdk::json_bool(document, "groundCollision", groundCollision);

    if (!DeformableConfig_validate(candidate, errorOut)) return false;
    *this = candidate;
    return true;
}

namespace {

class XpbdProvider final : public IDeformableProvider {
public:
    explicit XpbdProvider(const DeformableConfig& config) : config_(config) {}

    DeformableProviderKind kind() const noexcept override {
        return DeformableProviderKind::Xpbd;
    }
    const DeformableConfig& config() const noexcept override { return config_; }

    DeformableBodyHandle create_body(const DeformableMeshDesc& desc,
                                     std::string& errorOut) override {
        if (desc.nodes.empty()) {
            errorOut = "deformable: empty mesh refused";
            return InvalidDeformableBody;
        }
        if (desc.nodes.size() > config_.maxNodes) {
            errorOut = "deformable: mesh exceeds maxNodes (" +
                       std::to_string(config_.maxNodes) + ")";
            return InvalidDeformableBody;
        }
        if (!desc.fixed.empty() && desc.fixed.size() != desc.nodes.size()) {
            errorOut = "deformable: fixed flags must match the node count";
            return InvalidDeformableBody;
        }
        if (!desc.stiffness.empty() && desc.stiffness.size() != desc.edges.size()) {
            errorOut = "deformable: per-edge stiffness must match the edge count";
            return InvalidDeformableBody;
        }
        for (const auto& edge : desc.edges) {
            if (edge.first >= desc.nodes.size() || edge.second >= desc.nodes.size()) {
                errorOut = "deformable: edge references an invalid node index";
                return InvalidDeformableBody;
            }
        }
        for (const float k : desc.stiffness) {
            if (!(k > 0.0f) || k > 1.0f) {
                errorOut = "deformable: per-edge stiffness must be in (0, 1]";
                return InvalidDeformableBody;
            }
        }

        Body body;
        body.nodes.resize(desc.nodes.size());
        for (std::size_t i = 0; i < desc.nodes.size(); ++i) {
            body.nodes[i].position = desc.nodes[i];
            body.nodes[i].predicted = desc.nodes[i];
            if (!desc.fixed.empty() && desc.fixed[i]) body.nodes[i].inverseMass = 0.0f;
        }
        body.edges.reserve(desc.edges.size());
        const float defaultAlpha = (1.0f - config_.stiffness) / config_.stiffness;
        for (std::size_t i = 0; i < desc.edges.size(); ++i) {
            const auto& edge = desc.edges[i];
            Edge e;
            e.a = edge.first;
            e.b = edge.second;
            e.restLength = glm::distance(body.nodes[e.a].position,
                                         body.nodes[e.b].position);
            // Per-edge stiffness override (node/beam chassis, §17 item 4);
            // empty list = the config stiffness for every edge.
            e.alpha = desc.stiffness.empty()
                ? defaultAlpha
                : (1.0f - desc.stiffness[i]) / desc.stiffness[i];
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
        Edge& edge = found->second.edges[edgeIndex];
        // stiffness == 0 = fully compliant (constraint deactivated — the part
        // separated); (0, 1] maps to the same alpha as create_body. A huge
        // alpha makes the XPBD correction ~0 without touching the persistence
        // of the Lagrange multiplier.
        const float k = glm::clamp(stiffness, 0.0f, 1.0f);
        edge.alpha = k <= 0.0f ? kSeparatedAlpha : (1.0f - k) / k;
    }

    void step(float dt) override {
        if (!(dt > 0.0f) || !std::isfinite(dt)) return;
        const float substep = dt / static_cast<float>(config_.substeps);
        // Compliance WITHOUT the dt^2 scaling: the classic PBD stiffness
        // interpretation (alpha = (1-k)/k), frame-rate independent — the
        // solver's behavior must not depend on the caller's substep choice.
        // (The XPBD dt^2 convention makes constraints absurdly soft at 60 Hz:
        // alpha ~= 758 for k=0.95, so a hanging chain stretches unboundedly.)
        for (auto& entry : bodies_) {
            Body& body = entry.second;
            for (int s = 0; s < config_.substeps; ++s) {
                integrate_substep(body, substep);
            }
            // Forces are accumulated until the END of the step() call (they
            // persist across all substeps), then cleared.
            for (Node& node : body.nodes) node.force = glm::vec3(0.0f);
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
        double total = 0.0;
        for (const Edge& edge : found->second.edges) {
            const glm::vec3 delta =
                found->second.nodes[edge.b].position - found->second.nodes[edge.a].position;
            total += std::fabs(glm::length(delta) - edge.restLength);
        }
        return static_cast<float>(total / static_cast<double>(found->second.edges.size()));
    }

private:
    void integrate_substep(Body& body, float dt) {
        // 1. Integrate: velocity update + predicted position. Forces persist
        //    across ALL substeps of the step() call (cleared in step()).
        for (Node& node : body.nodes) {
            if (node.inverseMass == 0.0f) continue;  // fixed node
            node.velocity += node.force * (node.inverseMass * dt);
            node.velocity += config_.gravity * dt;
            node.velocity *= (1.0f - config_.damping);
            node.predicted = node.position + node.velocity * dt;
        }

        // 2. XPBD distance-constraint solve (fixed iteration order).
        for (int iteration = 0; iteration < config_.solverIterations; ++iteration) {
            for (Edge& edge : body.edges) {
                solve_edge(body, edge);
            }
        }

        // 3. Finalize: velocity from the position delta, commit positions.
        for (Node& node : body.nodes) {
            if (node.inverseMass == 0.0f) continue;
            node.velocity = (node.predicted - node.position) / dt;
            node.position = node.predicted;
        }

        // 4. Ground collision on the COMMITTED position (after the velocity
        //    is derived, so the bounce is not clobbered by finalize): clamp
        //    the plane and reflect the downward velocity by `bounce`.
        if (config_.groundCollision) {
            for (Node& node : body.nodes) {
                if (node.inverseMass == 0.0f) continue;
                if (node.position.y < config_.groundY) {
                    node.position.y = config_.groundY;
                    if (node.velocity.y < 0.0f)
                        node.velocity.y = -node.velocity.y * config_.bounce;
                }
            }
        }
    }

    static void solve_edge(Body& body, Edge& edge) {
        Node& a = body.nodes[edge.a];
        Node& b = body.nodes[edge.b];
        const float wSum = a.inverseMass + b.inverseMass;
        if (wSum <= 0.0f) return;  // both fixed

        glm::vec3 delta = b.predicted - a.predicted;
        float length = glm::length(delta);
        glm::vec3 direction;
        if (length > kEpsilon) {
            direction = delta / length;
        } else {
            // Degenerate (nodes coincide): deterministic fallback along +X.
            direction = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        const float constraint = length - edge.restLength;
        const float denominator = wSum + edge.alpha;
        if (denominator <= kEpsilon) return;
        const float dLambda = (-constraint - edge.alpha * edge.lambda) / denominator;
        edge.lambda += dLambda;

        a.predicted -= direction * (a.inverseMass * dLambda);
        b.predicted += direction * (b.inverseMass * dLambda);
    }

    DeformableConfig config_;
    std::unordered_map<DeformableBodyHandle, Body> bodies_;
    DeformableBodyHandle nextHandle_{ InvalidDeformableBody };
};

}  // namespace

std::unique_ptr<IDeformableProvider> create_deformable_provider(
    DeformableProviderKind kind, const DeformableConfig& config,
    std::string& errorOut) {
    if (kind == DeformableProviderKind::Femfx) {
        errorOut = "deformable provider: FEMFX is a specialized opt-in plugin "
                   "not vendored (DEPENDENCY_POLICY); implement the same "
                   "IDeformableProvider seam to add it";
        return nullptr;
    }
    std::string validateError;
    if (!DeformableConfig_validate(config, validateError)) {
        errorOut = validateError;
        return nullptr;
    }
    return std::make_unique<XpbdProvider>(config);
}

}  // namespace Engine::Deformable
