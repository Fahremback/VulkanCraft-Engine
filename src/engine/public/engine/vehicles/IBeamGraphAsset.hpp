#pragma once

// BeamGraphAsset (FALTANTES §17 item 4): a node/beam deformable chassis in the
// style of classic soft-body vehicle simulators (BeamNG-likes). The chassis is
// a graph of NODES (mass points) connected by BEAMS (distance constraints with
// per-beam stiffness); wheels are mounted on nodes and the whole assembly is
// solved by position-based dynamics (XPBD — the same solver behind
// IDeformableProvider, §16 item 3). The asset is PURE DATA (JSON, versioned,
// all-or-nothing) — it never touches physics or rendering itself.
//
// The runtime (IGameplayRuntime::create_beam_vehicle) builds a deformable body
// from the graph, applies wheel suspension/drive/brake forces at the mount
// nodes, steps the XPBD solver, and exposes the deformed node positions — a
// chassis that BENTHES under load instead of a rigid body.
//
// Self-contained (glm only). load_from_json / to_json / validate are
// implemented by the SDK adapter (src/engine/sdk/BeamGraphAsset.cpp).

#include "engine/vehicles/IVehicleAsset.hpp"  // WheelComponent (suspension/tire/steer), VehicleProviderKind

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace vehicles {

// One mass point of the deformable chassis. `position` is the REST position in
// the chassis's local space (the runtime applies asset.position/rotation at
// assembly). `fixed` anchors the node (inverse mass 0 — the XPBD solver never
// moves it; e.g. a trailer hitch or a crane mount).
struct BeamNode {
    glm::vec3 position{ 0.0f };
    bool fixed{ false };
};

// One beam: a distance constraint between two nodes with its OWN stiffness in
// (0, 1] (1 = rigid, lower = more compliant — the node/beam style: a soft
// crumple zone vs a rigid roll cage). The solver uses the config stiffness for
// beams without an explicit override.
struct Beam {
    std::uint32_t a{ 0 };
    std::uint32_t b{ 0 };
    float stiffness{ 0.9f };  // (0, 1]; per-edge override
};

// A wheel bolted to a chassis node. `node` is the beam-graph node the wheel
// hangs from; the wheel's suspension/drive/brake forces are applied to that
// node (the chassis deforms under load). The wheel geometry reuses the public
// WheelComponent (radius, suspension spring/damper, tire grip, drive/brake/
// steer forces).
struct BeamWheelMount {
    std::uint32_t node{ 0 };      // beam-graph node index
    WheelComponent wheel;         // suspension/tire/steer of this wheel
    bool steering{ true };        // steer input rotates this wheel's drive
    bool driven{ true };          // throttle drives this wheel
};

// Solver settings for the XPBD solve of the beam chassis (mirrors the
// DeformableConfig subset the runtime maps into the provider).
struct BeamSolverConfig {
    int substeps{ 2 };          // position-dynamics substeps per step [1,16]
    int solverIterations{ 8 };  // XPBD iterations per substep [1,64]
    float stiffness{ 0.8f };    // default beam stiffness in (0, 1]
    float damping{ 0.1f };      // velocity damping in [0, 1)
    glm::vec3 gravity{ 0.0f, -9.81f, 0.0f };
};

// The node/beam deformable chassis definition: the composition of the public
// components. position/rotation are the assembly (spawn) transform of the
// chassis origin.
struct BeamGraphAsset {
    std::string id;   // stable project id; derived from name when absent
    std::string name;
    int version{ 1 };

    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    // Physics provider behind this vehicle (FALTANTES §17 items 5/6): the
    // asset selects EXACTLY ONE provider (jolt|chrono|jsbsim). Only jolt is
    // vendored today; chrono/jsbsim are refused with a diagnostic at
    // creation (never a silent fallback). Defaults to Jolt (legacy behavior).
    VehicleProviderKind provider{ VehicleProviderKind::Jolt };

    // Total chassis mass (kg). The wheel suspension/drive constants reuse the
    // rigid-vehicle WheelComponent values, which are tuned for a ~1000+ kg
    // body; the runtime distributes this mass across the nodes and scales the
    // per-node forces accordingly (each XPBD node is unit mass).
    float mass{ 1200.0f };

    std::vector<BeamNode> nodes;
    std::vector<Beam> beams;
    std::vector<BeamWheelMount> wheels;
    // Occupant seats (FALTANTES §17 item 8): pose derived from the DEFORMED
    // node frame. Empty = no occupants.
    std::vector<VehicleSeat> seats;
    // Energy/fuel/controls (FALTANTES §17 item 7). Defaults disable the
    // systems (no consumption) and use the identity control mapping.
    VehiclePower power;
    BeamSolverConfig solver;

    // All-or-nothing: refuses malformed documents with a diagnostic (never
    // clamps or partially applies). Implemented by the SDK adapter.
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    // Bit-exact: to_json() round-trips every field (%.9g float emission).
    std::string to_json() const;
    bool validate(std::string& errorOut) const;
};

}  // namespace vehicles
}  // namespace engine
