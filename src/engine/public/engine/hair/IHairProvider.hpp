#pragma once

// IHairProvider (FALTANTES §18 item 10): the PUBLIC hair/fur contract. Hair is
// a set of STRANDS — chains of control points rooted at the scalp/skin; the
// provider simulates them with position-based dynamics (the XPBD family:
// Müller/Macklin/Chentanez "XPBD: Position-Based Simulation of Compliant
// Constrained Dynamics"), deterministically and headless, with LOD.
//
// PLUGIN ARCHITECTURE: the item defines TressFX (AMD, GPU hair/fur renderer +
// simulation) as an OPTIONAL specialized plugin behind this contract.
// `create_hair_provider(kind, ...)` is the seam:
//   - StrandSolver — implemented (`src/engine/sdk/StrandHair.cpp`, the only TU
//     that crosses into the solver): self-contained, deterministic (fixed
//     strand/segment iteration order, no randomness), headless.
//   - Tressfx — GPU renderer-coupled plugin NOT vendored (DEPENDENCY_POLICY
//     lists it as an opt-in asset choice); the factory REFUSES it with a
//     diagnostic instead of silently falling back, so a missing plugin is
//     never mistaken for working hair.
//
// LOD (level of detail): per body, `set_lod(relevance)` [0, 1] reduces the
// ACTIVE strand count to max(1, round(strandCount * relevance)) — strands
// beyond the budget are FROZEN (they keep their last simulated positions and
// are not stepped), which is the determinism-friendly strand LOD; the renderer
// simply stops drawing the frozen strands. All-or-nothing: out-of-range
// relevance is refused.
//
// Self-contained (std + glm). Deterministic: identical config + strand desc +
// step sequence produce bit-identical node states.

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Hair {

enum class HairProviderKind : std::uint8_t {
    StrandSolver,  // position-based strand dynamics (implemented)
    Tressfx        // AMD TressFX GPU plugin (specialized, opt-in, not vendored)
};

// One strand: control points in world space; point[0] is the ROOT (fixed to
// the skin/scalp — infinite mass, never moved by the solver).
struct HairStrandDesc {
    std::vector<std::vector<glm::vec3>> strands;
};

// Strand-dynamics configuration, data-driven via JSON (all-or-nothing:
// out-of-range values are REFUSED with a diagnostic, never clamped — mirroring
// the other gameplay/procgen configs).
struct HairConfig {
    std::size_t maxStrands{ 4096 };    // total strand cap per body (refused above)
    std::size_t strandMaxNodes{ 256 }; // control-point cap per strand [2, 1024]
    int substeps{ 2 };                 // position-dynamics substeps per step() [1,16]
    int solverIterations{ 8 };         // XPBD constraint iterations per substep [1,64]
    float stiffness{ 0.8f };           // segment constraint stiffness in (0, 1]
    float bendStiffness{ 0.6f };       // bending stiffness in [0, 1] — 0 = a
                                       // perfectly flexible rope (the chain
                                       // hangs straight down), higher = hair-
                                       // like resistance to curvature (the
                                       // strand keeps its rest curvature).
    bool pinRootDirection{ false };    // hard-pin the FIRST segment's
                                       // direction to the rest root direction
                                       // (the follicle: hair grows out of the
                                       // scalp at a fixed angle; without it a
                                       // straight strand swings as a rigid
                                       // pendulum and droop can't distinguish
                                       // floppy vs stiff hair).
    float damping{ 0.1f };             // velocity damping in [0, 1)
    glm::vec3 gravity{ 0.0f, -9.8f, 0.0f };  // per-substep acceleration
    bool groundCollision{ true };      // clamp nodes to groundY + bounce
    float groundY{ 0.0f };             // ground plane height
    float bounce{ 0.0f };              // ground restitution in [0, 1]

    // JSON keys: maxStrands / strandMaxNodes / substeps / solverIterations /
    // stiffness / bendStiffness / pinRootDirection / damping / gravityX /
    // gravityY / gravityZ / groundCollision / groundY / bounce.
    bool load_from_json(const std::string& json, std::string& errorOut);
};

using HairStrandBodyHandle = std::uint64_t;
constexpr HairStrandBodyHandle InvalidHairBody = 0;

class IHairProvider {
public:
    virtual ~IHairProvider() = default;

    virtual HairProviderKind kind() const noexcept = 0;
    virtual const HairConfig& config() const noexcept = 0;

    // Creates a hair body from the strand description. Refuses (false +
    // diagnostic) empty strand sets, strands with fewer than 2 points, strand
    // point counts over the per-strand cap, or a total strand count over the
    // config cap.
    virtual HairStrandBodyHandle create_strand_body(const HairStrandDesc& desc,
                                                    std::string& errorOut) = 0;
    virtual bool destroy_strand_body(HairStrandBodyHandle body) = 0;
    virtual std::size_t body_count() const noexcept = 0;

    // Applies a force to one node (accumulated until the next step(), then
    // cleared). No-op for an invalid body/strand/node.
    virtual void apply_force(HairStrandBodyHandle body, std::uint32_t strand,
                             std::uint32_t node, const glm::vec3& force) = 0;

    // LOD: sets the active-strand budget for the body to
    // max(1, round(strandCount * relevance)). Strands beyond the budget are
    // frozen (kept at their last simulated positions, not stepped). Refuses
    // (false) out-of-range relevance.
    virtual bool set_lod(HairStrandBodyHandle body, float relevance,
                         std::string& errorOut) = 0;

    // Advances the simulation: for each substep, integrate (gravity + forces
    // + damping), solve the XPBD segment constraints with the configured
    // iteration count, apply the ground collision, update velocities. Only
    // ACTIVE strands are stepped. Deterministic. dt must be > 0.
    virtual void step(float dt) = 0;

    // Observability (rendering, telemetry).
    virtual std::size_t strand_count(HairStrandBodyHandle body) const noexcept = 0;
    virtual std::size_t active_strand_count(HairStrandBodyHandle body) const noexcept = 0;
    virtual std::size_t node_count(HairStrandBodyHandle body,
                                   std::uint32_t strand) const noexcept = 0;
    virtual glm::vec3 node_position(HairStrandBodyHandle body,
                                    std::uint32_t strand,
                                    std::uint32_t node) const noexcept = 0;
    virtual glm::vec3 node_velocity(HairStrandBodyHandle body,
                                    std::uint32_t strand,
                                    std::uint32_t node) const noexcept = 0;
    // Average |length - restLength| over the ACTIVE segments — the
    // solver-convergence observable (0 = perfectly satisfied).
    virtual float constraint_error(HairStrandBodyHandle body) const noexcept = 0;
};

// The plugin seam: StrandSolver returns the implemented solver; Tressfx is
// REFUSED with a diagnostic (specialized GPU opt-in plugin, not vendored — see
// DEPENDENCY_POLICY). Never silently falls back.
std::unique_ptr<IHairProvider> create_hair_provider(HairProviderKind kind,
                                                    const HairConfig& config,
                                                    std::string& errorOut);

}  // namespace Engine::Hair
