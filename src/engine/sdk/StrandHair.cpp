// StrandHair.cpp
//
// The StrandSolver plugin behind IHairProvider (FALTANTES §18 item 10): hair
// as a set of rooted STRANDS (chains of control points) solved with the same
// XPBD family as the deformable solver (Müller/Macklin/Chentanez "XPBD") —
// self-contained, deterministic (fixed strand/segment iteration order, no
// randomness), headless. TressFX (AMD GPU hair/fur renderer) is the OPTIONAL
// specialized plugin behind the same contract; the factory REFUSES it with a
// diagnostic (not vendored — DEPENDENCY_POLICY), never a silent fallback.
//
//   integrate — per substep: velocity += dt*(gravity + force/mass), damping
//               applied, predicted position x = p + dt*v. The strand ROOT
//               (inverse mass 0) never moves.
//   solve     — solverIterations XPBD passes over the strand segments: each
//               segment's compliance-scaled Lagrange multiplier is updated
//               and the endpoints projected by inverse mass.
//   finalize  — v = (x - p)/dt; p = x; accumulated forces cleared.
//   ground    — nodes below groundY are clamped back with bounce restitution
//               (on the committed position, after the velocity is derived).
//
// LOD: set_lod(relevance) freezes the strands beyond
// max(1, round(strandCount * relevance)) — frozen strands keep their last
// simulated positions and are NOT stepped (determinism-friendly strand LOD).
//
// The ONLY TU that crosses into the strand solver; the public contract is
// everything the caller sees.
#include "engine/hair/IHairProvider.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <limits>
#include <unordered_map>

namespace Engine::Hair {

namespace {

constexpr float kEpsilon = 1.0e-6f;

bool HairConfig_validate(const HairConfig& config, std::string& errorOut) {
    if (config.maxStrands < 1 || config.maxStrands > 1000000) {
        errorOut = "hair config: maxStrands must be in [1, 1000000]";
        return false;
    }
    if (config.strandMaxNodes < 2 || config.strandMaxNodes > 1024) {
        errorOut = "hair config: strandMaxNodes must be in [2, 1024]";
        return false;
    }
    if (config.substeps < 1 || config.substeps > 16) {
        errorOut = "hair config: substeps must be in [1, 16]";
        return false;
    }
    if (config.solverIterations < 1 || config.solverIterations > 64) {
        errorOut = "hair config: solverIterations must be in [1, 64]";
        return false;
    }
    if (config.stiffness <= 0.0f || config.stiffness > 1.0f) {
        errorOut = "hair config: stiffness must be in (0, 1]";
        return false;
    }
    if (config.bendStiffness < 0.0f || config.bendStiffness > 1.0f) {
        errorOut = "hair config: bendStiffness must be in [0, 1]";
        return false;
    }
    if (config.damping < 0.0f || config.damping >= 1.0f) {
        errorOut = "hair config: damping must be in [0, 1)";
        return false;
    }
    if (config.bounce < 0.0f || config.bounce > 1.0f) {
        errorOut = "hair config: bounce must be in [0, 1]";
        return false;
    }
    if (!std::isfinite(config.gravity.x) || !std::isfinite(config.gravity.y) ||
        !std::isfinite(config.gravity.z) || !std::isfinite(config.groundY)) {
        errorOut = "hair config: gravity/groundY must be finite";
        return false;
    }
    return true;
}

struct Strand {
    std::vector<glm::vec3> position;     // committed positions
    std::vector<glm::vec3> predicted;    // per-substep predicted positions
    std::vector<glm::vec3> velocity;
    std::vector<glm::vec3> forces;       // accumulated, cleared per step()
    std::vector<float> restLengths;      // size = nodes - 1
    std::vector<float> lambda;           // XPBD multipliers (persist)
    std::vector<float> restCurv;         // size = nodes - 2 (bending)
    std::vector<float> inverseMass;      // [0] = 0 (root fixed)
    glm::vec3 restRootDir{ 1.0f, 0.0f, 0.0f };  // unit, first segment
    bool active{ true };
};

struct Body {
    std::vector<Strand> strands;
    std::size_t activeCount{ 0 };
};

// Compliance alpha per edge: (1 - k) / k — the SAME interpretation as the
// deformable solver (frame-rate independent; a small alpha resists stretch).
float edge_alpha(float stiffness) { return (1.0f - stiffness) / stiffness; }

class StrandHairImpl final : public IHairProvider {
public:
    explicit StrandHairImpl(const HairConfig& config, std::string& errorOut)
        : config_(config) {
        if (!HairConfig_validate(config_, errorOut)) return;
        valid_ = true;
    }

    HairProviderKind kind() const noexcept override {
        return HairProviderKind::StrandSolver;
    }

    const HairConfig& config() const noexcept override { return config_; }

    HairStrandBodyHandle create_strand_body(const HairStrandDesc& desc,
                                            std::string& errorOut) override {
        if (!valid_) {
            errorOut = "hair provider: invalid config";
            return InvalidHairBody;
        }
        if (desc.strands.empty()) {
            errorOut = "hair provider: no strands";
            return InvalidHairBody;
        }
        if (desc.strands.size() > config_.maxStrands) {
            errorOut = "hair provider: strand count over the config cap";
            return InvalidHairBody;
        }
        Body body;
        body.strands.reserve(desc.strands.size());
        for (const std::vector<glm::vec3>& points : desc.strands) {
            if (points.size() < 2) {
                errorOut = "hair provider: strand needs at least 2 points";
                return InvalidHairBody;
            }
            if (points.size() > config_.strandMaxNodes) {
                errorOut = "hair provider: strand over the node cap";
                return InvalidHairBody;
            }
            for (const glm::vec3& p : points) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
                    !std::isfinite(p.z)) {
                    errorOut = "hair provider: non-finite strand point";
                    return InvalidHairBody;
                }
            }
            Strand strand;
            const std::size_t n = points.size();
            strand.position = points;
            strand.predicted = points;
            strand.velocity.assign(n, glm::vec3(0.0f));
            strand.forces.assign(n, glm::vec3(0.0f));
            strand.restLengths.resize(n - 1);
            strand.lambda.assign(n - 1, 0.0f);
            strand.restCurv.assign(n > 2 ? n - 2 : 0, 0.0f);
            strand.inverseMass.assign(n, 1.0f);
            strand.inverseMass[0] = 0.0f;  // root fixed
            for (std::size_t i = 0; i + 1 < n; ++i) {
                strand.restLengths[i] =
                    glm::length(points[i + 1] - points[i]);
                if (strand.restLengths[i] <= kEpsilon) {
                    errorOut = "hair provider: degenerate segment (zero length)";
                    return InvalidHairBody;
                }
            }
            for (std::size_t j = 1; j + 1 < n; ++j) {
                // Rest curvature (second difference): 0 for a straight strand.
                strand.restCurv[j - 1] = glm::length(
                    points[j - 1] - 2.0f * points[j] + points[j + 1]);
            }
            strand.restRootDir = glm::normalize(points[1] - points[0]);
            body.strands.push_back(std::move(strand));
        }
        body.activeCount = body.strands.size();
        const HairStrandBodyHandle handle = nextBodyHandle_++;
        bodies_[handle] = std::move(body);
        return handle;
    }

    bool destroy_strand_body(HairStrandBodyHandle body) override {
        return bodies_.erase(body) != 0;
    }

    std::size_t body_count() const noexcept override { return bodies_.size(); }

    void apply_force(HairStrandBodyHandle body, std::uint32_t strand,
                     std::uint32_t node, const glm::vec3& force) override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end()) return;
        Body& b = found->second;
        if (strand >= b.strands.size()) return;
        Strand& s = b.strands[strand];
        if (node >= s.position.size()) return;
        s.forces[node] += force;
    }

    bool set_lod(HairStrandBodyHandle body, float relevance,
                 std::string& errorOut) override {
        if (!std::isfinite(relevance) || relevance < 0.0f || relevance > 1.0f) {
            errorOut = "hair provider: relevance must be in [0, 1]";
            return false;
        }
        const auto found = bodies_.find(body);
        if (found == bodies_.end()) return false;
        Body& b = found->second;
        const std::size_t active = std::max<std::size_t>(
            1, static_cast<std::size_t>(
                   std::lround(static_cast<float>(b.strands.size()) *
                               relevance)));
        b.activeCount = active;
        for (std::size_t i = 0; i < b.strands.size(); ++i) {
            b.strands[i].active = i < active;
        }
        return true;
    }

    void step(float dt) override {
        if (!valid_ || dt <= 0.0f) return;
        const float subDt = dt / static_cast<float>(config_.substeps);
        for (auto& [handle, body] : bodies_) {
            for (int substep = 0; substep < config_.substeps; ++substep) {
                for (Strand& strand : body.strands) {
                    if (!strand.active) continue;
                    integrate(strand, subDt);
                }
                for (int iteration = 0; iteration < config_.solverIterations;
                     ++iteration) {
                    for (Strand& strand : body.strands) {
                        if (!strand.active) continue;
                        solve(strand);
                    }
                }
                for (Strand& strand : body.strands) {
                    if (!strand.active) continue;
                    finalize(strand, subDt);
                }
            }
            if (config_.groundCollision) {
                for (Strand& strand : body.strands) {
                    if (!strand.active) continue;
                    for (std::size_t i = 1; i < strand.position.size(); ++i) {
                        if (strand.position[i].y < config_.groundY) {
                            strand.position[i].y = config_.groundY;
                            if (strand.velocity[i].y < 0.0f) {
                                strand.velocity[i].y =
                                    -strand.velocity[i].y * config_.bounce;
                            }
                        }
                    }
                }
            }
            // Forces are cleared once per step() (they persist across the
            // substeps, like the deformable solver).
            for (Strand& strand : body.strands) {
                std::fill(strand.forces.begin(), strand.forces.end(),
                          glm::vec3(0.0f));
            }
        }
    }

    std::size_t strand_count(HairStrandBodyHandle body) const noexcept override {
        const auto found = bodies_.find(body);
        return found == bodies_.end() ? 0 : found->second.strands.size();
    }

    std::size_t active_strand_count(HairStrandBodyHandle body) const noexcept override {
        const auto found = bodies_.find(body);
        return found == bodies_.end() ? 0 : found->second.activeCount;
    }

    std::size_t node_count(HairStrandBodyHandle body,
                           std::uint32_t strand) const noexcept override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end() || strand >= found->second.strands.size())
            return 0;
        return found->second.strands[strand].position.size();
    }

    glm::vec3 node_position(HairStrandBodyHandle body, std::uint32_t strand,
                            std::uint32_t node) const noexcept override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end() || strand >= found->second.strands.size())
            return glm::vec3(0.0f);
        const Strand& s = found->second.strands[strand];
        return node < s.position.size() ? s.position[node] : glm::vec3(0.0f);
    }

    glm::vec3 node_velocity(HairStrandBodyHandle body, std::uint32_t strand,
                            std::uint32_t node) const noexcept override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end() || strand >= found->second.strands.size())
            return glm::vec3(0.0f);
        const Strand& s = found->second.strands[strand];
        return node < s.velocity.size() ? s.velocity[node] : glm::vec3(0.0f);
    }

    float constraint_error(HairStrandBodyHandle body) const noexcept override {
        const auto found = bodies_.find(body);
        if (found == bodies_.end()) return 0.0f;
        float sum = 0.0f;
        std::size_t count = 0;
        for (const Strand& s : found->second.strands) {
            if (!s.active) continue;
            for (std::size_t i = 0; i + 1 < s.position.size(); ++i) {
                sum += std::fabs(
                    glm::length(s.position[i + 1] - s.position[i]) -
                    s.restLengths[i]);
                ++count;
            }
        }
        return count == 0 ? 0.0f : sum / static_cast<float>(count);
    }

private:
    void integrate(Strand& strand, float dt) const {
        for (std::size_t i = 1; i < strand.position.size(); ++i) {
            strand.velocity[i] +=
                strand.forces[i] * (strand.inverseMass[i] * dt);
            strand.velocity[i] += config_.gravity * dt;
            strand.velocity[i] *= (1.0f - config_.damping);
            strand.predicted[i] =
                strand.position[i] + strand.velocity[i] * dt;
        }
    }

    void solve(Strand& strand) const {
        const float alpha = edge_alpha(config_.stiffness);
        for (std::size_t i = 0; i + 1 < strand.position.size(); ++i) {
            const float wSum = strand.inverseMass[i] +
                               strand.inverseMass[i + 1];
            if (wSum <= 0.0f) continue;  // both fixed
            glm::vec3 delta = strand.predicted[i + 1] - strand.predicted[i];
            const float length = glm::length(delta);
            glm::vec3 direction;
            if (length > kEpsilon) {
                direction = delta / length;
            } else {
                // Degenerate (nodes coincide): deterministic fallback along +X.
                direction = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            const float constraint = length - strand.restLengths[i];
            const float denominator = wSum + alpha;
            if (denominator <= kEpsilon) continue;
            const float dLambda =
                (-constraint - alpha * strand.lambda[i]) / denominator;
            strand.lambda[i] += dLambda;
            strand.predicted[i] -=
                direction * (strand.inverseMass[i] * dLambda);
            strand.predicted[i + 1] +=
                direction * (strand.inverseMass[i + 1] * dLambda);
        }
        // Root direction pin (the follicle): the first segment stays along
        // its rest direction — hard constraint, only node 1 moves (root has
        // infinite mass). Without it a straight strand would swing as a rigid
        // pendulum, so droop could never distinguish floppy vs stiff hair.
        if (config_.pinRootDirection && strand.position.size() > 1) {
            const glm::vec3 delta = strand.predicted[1] - strand.predicted[0];
            const glm::vec3 along = strand.restRootDir *
                                    glm::dot(delta, strand.restRootDir);
            strand.predicted[1] = strand.predicted[0] + along;
        }
        // Bending (hair-like resistance to curvature): the classic PBD
        // second-difference constraint |p[j-1] - 2 p[j] + p[j+1]| pulled back
        // to the strand's rest curvature. bendStiffness 0 = perfectly flexible
        // (the chain hangs straight down); higher = keeps the rest shape.
        if (config_.bendStiffness <= 0.0f) return;
        for (std::size_t j = 1; j + 1 < strand.position.size(); ++j) {
            const glm::vec3 d = strand.predicted[j - 1] -
                                2.0f * strand.predicted[j] +
                                strand.predicted[j + 1];
            const float length = glm::length(d);
            const float rest = strand.restCurv[j - 1];
            const float constraint = length - rest;
            if (std::fabs(constraint) <= kEpsilon) continue;
            glm::vec3 direction;
            if (length > kEpsilon) {
                direction = d / length;
            } else {
                // Degenerate (collinear): deterministic fallback along +Y.
                direction = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            const float wSum = strand.inverseMass[j - 1] +
                               4.0f * strand.inverseMass[j] +
                               strand.inverseMass[j + 1];
            if (wSum <= kEpsilon) continue;
            const float delta =
                -constraint * config_.bendStiffness / wSum;
            strand.predicted[j - 1] +=
                direction * (strand.inverseMass[j - 1] * delta);
            strand.predicted[j] -=
                direction * (strand.inverseMass[j] * 2.0f * delta);
            strand.predicted[j + 1] +=
                direction * (strand.inverseMass[j + 1] * delta);
        }
    }

    static void finalize(Strand& strand, float dt) {
        for (std::size_t i = 1; i < strand.position.size(); ++i) {
            strand.velocity[i] =
                (strand.predicted[i] - strand.position[i]) / dt;
            strand.position[i] = strand.predicted[i];
        }
    }

    HairConfig config_;
    bool valid_{ false };
    std::unordered_map<HairStrandBodyHandle, Body> bodies_;
    HairStrandBodyHandle nextBodyHandle_{ 1 };
};

}  // namespace

bool HairConfig::load_from_json(const std::string& json,
                                std::string& errorOut) {
    engine::sdk::JsonValue document;
    if (!engine::sdk::json_parse(json, document, errorOut)) return false;
    if (!document.is_object()) {
        errorOut = "hair config must be a JSON object";
        return false;
    }
    // C4700 guard (findings #72/#74): defaults read the MEMBERS with this->.
    const double maxStrands = engine::sdk::json_number(
        document, "maxStrands", static_cast<double>(this->maxStrands));
    const double strandMaxNodes = engine::sdk::json_number(
        document, "strandMaxNodes", static_cast<double>(this->strandMaxNodes));
    const double substeps = engine::sdk::json_number(
        document, "substeps", static_cast<double>(this->substeps));
    const double solverIterations = engine::sdk::json_number(
        document, "solverIterations",
        static_cast<double>(this->solverIterations));
    const double stiffness = engine::sdk::json_number(
        document, "stiffness", static_cast<double>(this->stiffness));
    const double bendStiffness = engine::sdk::json_number(
        document, "bendStiffness", static_cast<double>(this->bendStiffness));
    const double damping = engine::sdk::json_number(
        document, "damping", static_cast<double>(this->damping));
    const double groundY = engine::sdk::json_number(
        document, "groundY", static_cast<double>(this->groundY));
    const double bounce = engine::sdk::json_number(
        document, "bounce", static_cast<double>(this->bounce));

    HairConfig candidate;
    candidate.maxStrands = static_cast<std::size_t>(maxStrands);
    candidate.strandMaxNodes = static_cast<std::size_t>(strandMaxNodes);
    candidate.substeps = static_cast<int>(substeps);
    candidate.solverIterations = static_cast<int>(solverIterations);
    candidate.stiffness = static_cast<float>(stiffness);
    candidate.bendStiffness = static_cast<float>(bendStiffness);
    candidate.damping = static_cast<float>(damping);
    candidate.groundY = static_cast<float>(groundY);
    candidate.bounce = static_cast<float>(bounce);
    candidate.pinRootDirection = engine::sdk::json_bool(
        document, "pinRootDirection", this->pinRootDirection);
    candidate.gravity.x = static_cast<float>(
        engine::sdk::json_number(document, "gravityX",
                                 static_cast<double>(this->gravity.x)));
    candidate.gravity.y = static_cast<float>(
        engine::sdk::json_number(document, "gravityY",
                                 static_cast<double>(this->gravity.y)));
    candidate.gravity.z = static_cast<float>(
        engine::sdk::json_number(document, "gravityZ",
                                 static_cast<double>(this->gravity.z)));
    candidate.groundCollision = engine::sdk::json_bool(
        document, "groundCollision", this->groundCollision);

    if (!HairConfig_validate(candidate, errorOut)) return false;
    *this = candidate;
    return true;
}

std::unique_ptr<IHairProvider> create_hair_provider(HairProviderKind kind,
                                                    const HairConfig& config,
                                                    std::string& errorOut) {
    if (kind == HairProviderKind::StrandSolver) {
        if (!HairConfig_validate(config, errorOut)) return nullptr;
        return std::make_unique<StrandHairImpl>(config, errorOut);
    }
    // TressFX is an opt-in GPU plugin (renderer-coupled), not vendored —
    // DEPENDENCY_POLICY. Refuse with a diagnostic: a missing plugin must never
    // look like working hair.
    errorOut = "hair provider: Tressfx is a specialized opt-in GPU plugin, "
               "not vendored (DEPENDENCY_POLICY)";
    return nullptr;
}

}  // namespace Engine::Hair
