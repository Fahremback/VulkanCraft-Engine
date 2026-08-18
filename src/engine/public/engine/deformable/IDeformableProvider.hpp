#pragma once

// IDeformableProvider (FALTANTES §16 item 3): the PUBLIC deformable-body
// contract. A deformable is a node/edge mesh solved by position-based
// dynamics (XPBD) — nodes carry a position and an inverse mass, edges are
// distance constraints with stiffness/compliance. The provider owns the
// bodies, integrates gravity + applied forces, solves the constraints with a
// fixed number of iterations, and exposes the result (node positions,
// velocities, constraint error) for rendering/collision/telemetry.
//
// PLUGIN ARCHITECTURE: the item defines XPBD and FEMFX as SPECIALIZED
// plugins behind this contract. `create_deformable_provider(kind, ...)` is
// the seam:
//   - Xpbd  — implemented (`src/engine/sdk/XpbdDeformable.cpp`, the only TU
//             that crosses into the solver): self-contained, deterministic
//             (fixed node/edge iteration order, no randomness), headless.
//   - Femfx — specialized volumetric plugin NOT vendored (DEPENDENCY_POLICY
//             lists it as an opt-in asset choice); the factory REFUSES it
//             with a diagnostic instead of silently falling back, so a
//             missing plugin is never mistaken for a working deformable.
//
// Self-contained: the contract depends only on the standard library + glm.
// Deterministic: identical provider config + body desc + step sequence
// produce bit-identical node states (item 5 formalizes the guarantee).

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Engine::Deformable {

enum class DeformableProviderKind : std::uint8_t {
    Xpbd,  // position-based dynamics (implemented)
    Femfx  // volumetric FEM plugin (specialized, opt-in, not vendored)
};

// One deformable body: a node/edge mesh. `nodes` are the rest positions in
// world space; `edges` are distance constraints (node index pairs); `fixed`
// marks anchored nodes (inverse mass 0 — infinite mass, never moved by the
// solver). `stiffness` is the OPTIONAL per-edge stiffness (FALTANTES §17 item
// 4 — node/beam chassis): must be empty (all edges use the config stiffness)
// or match edges.size() with values in (0, 1]. Deterministic solver order:
// nodes and edges are processed in the given order.
struct DeformableMeshDesc {
    std::vector<glm::vec3> nodes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    std::vector<bool> fixed;  // must match nodes.size() (false = free)
    std::vector<float> stiffness;  // optional per-edge stiffness (empty = config)
};

// XPBD solver configuration, data-driven via JSON (all-or-nothing: out-of-
// range values are REFUSED with a diagnostic, never clamped — mirroring the
// other gameplay/procgen configs).
struct DeformableConfig {
    std::size_t maxNodes{ 4096 };    // body node cap (refused above)
    int substeps{ 2 };               // position-dynamics substeps per step() [1,16]
    int solverIterations{ 8 };       // XPBD constraint iterations per substep [1,64]
    float stiffness{ 0.8f };         // constraint stiffness in (0, 1]
    float damping{ 0.1f };           // velocity damping in [0, 1)
    glm::vec3 gravity{ 0.0f, -9.8f, 0.0f };  // per-substep acceleration
    bool groundCollision{ true };    // clamp nodes to groundY + bounce
    float groundY{ 0.0f };           // ground plane height
    float bounce{ 0.0f };            // ground restitution in [0, 1]

    // JSON keys: maxNodes / substeps / solverIterations / stiffness /
    // damping / gravity / groundCollision / groundY / bounce.
    bool load_from_json(const std::string& json, std::string& errorOut);
};

using DeformableBodyHandle = std::uint64_t;
constexpr DeformableBodyHandle InvalidDeformableBody = 0;

class IDeformableProvider {
public:
    virtual ~IDeformableProvider() = default;

    virtual DeformableProviderKind kind() const noexcept = 0;
    virtual const DeformableConfig& config() const noexcept = 0;

    // Creates a body from the mesh description. Refuses (false + diagnostic)
    // an empty mesh, nodes over the config cap, fixed flags mismatched with
    // the nodes, or edges referencing invalid node indices.
    virtual DeformableBodyHandle create_body(const DeformableMeshDesc& desc,
                                             std::string& errorOut) = 0;
    virtual bool destroy_body(DeformableBodyHandle body) = 0;
    virtual std::size_t body_count() const noexcept = 0;

    // Applies a force to one node (accumulated until the next step(), then
    // cleared). No-op for an invalid body/node.
    virtual void apply_force(DeformableBodyHandle body, std::uint32_t node,
                             const glm::vec3& force) = 0;

    // FALTANTES §17 item 9 (vehicle damage): changes an edge's stiffness AFTER
    // creation. stiffness == 0 fully complies (the constraint is deactivated —
    // the damaged/separated part no longer holds the mesh together); (0, 1]
    // sets the edge's compliance. No-op for an invalid body/edge index.
    virtual void set_edge_stiffness(DeformableBodyHandle body,
                                    std::uint32_t edgeIndex,
                                    float stiffness) = 0;

    // Advances the simulation: for each substep, integrate (gravity + forces
    // + damping), solve the XPBD distance constraints with the configured
    // iteration count, apply the ground collision, update velocities.
    // Deterministic. dt must be > 0.
    virtual void step(float dt) = 0;

    // Observability (rendering, collision, telemetry).
    virtual std::size_t node_count(DeformableBodyHandle body) const noexcept = 0;
    virtual glm::vec3 node_position(DeformableBodyHandle body,
                                    std::uint32_t node) const noexcept = 0;
    virtual glm::vec3 node_velocity(DeformableBodyHandle body,
                                    std::uint32_t node) const noexcept = 0;
    // Average |length - restLength| over the body's distance constraints —
    // the solver-convergence observable (0 = perfectly satisfied).
    virtual float constraint_error(DeformableBodyHandle body) const noexcept = 0;
};

// The plugin seam: Xpbd returns the implemented solver; Femfx is REFUSED
// with a diagnostic (specialized opt-in plugin, not vendored — see
// DEPENDENCY_POLICY). Never silently falls back.
std::unique_ptr<IDeformableProvider> create_deformable_provider(
    DeformableProviderKind kind, const DeformableConfig& config,
    std::string& errorOut);

}  // namespace Engine::Deformable
