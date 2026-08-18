#pragma once

// IOfflineFarm (FALTANTES §18 item 11): the PUBLIC seam for the OFFLINE
// physics/learning farm. MuJoCo, MuJoCo MPC and DeepMimic generate or
// validate motion assets OUTSIDE the game (headless, offline — the pipeline
// in ACELERADORES_ANIMACAO_FISICA_PROCGEN.md: rig + objective + training
// environment -> generate/optimize motion -> validate stability/contacts ->
// export clip, small trajectory policy or parameter table -> cooker validates
// and signs -> runtime consumes the deterministic asset). NONE of the training
// environment is ever linked into the runtime — the farm tools are REFERENCE
// ONLY (the shape-ml / minecraft-spider pattern: never compiled, per
// DEPENDENCY_POLICY "treinamento, não dependência do jogo").
//
// What the runtime DOES consume is the COOKED asset: a canonical, deterministic
// blob (clip samples, a small trajectory policy, or a parameter table) with a
// stable signature. `IFarmCooker` is the seam that validates and signs it:
//   - `cook` builds a CookedFarmAsset from a canonical payload, computes the
//     signature, and refuses (false + diagnostic) any non-canonical input —
//     all-or-nothing, never clamped, mirroring the other gameplay configs.
//   - `verify` recomputes the signature and checks the payload is intact and
//     matches the signed header — the cooker gate before the runtime trusts
//     an asset produced by the farm.
//   - `create_farm_cooker(kind, ...)` REFUSES any training-env kind (MuJoCo,
//     MuJoCoMpc, DeepMimic) with a diagnostic: those tools belong to the
//     offline farm, and a missing/refused training backend must never look
//     like a working runtime simulator.
//
// The signature is a deterministic FNV-1a 64 over the CANONICAL payload bytes
// (a fixed per-kind serialization: little-endian fields in declaration order,
// exact float bit patterns — never a text/JSON re-encode, which could differ
// across locales). Same payload -> same signature, bit-exact, across runs and
// machines.
//
// Self-contained (std + glm). Deterministic. Headless.

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Farm {

// What kind of asset the offline farm produced. The runtime's anim/physics
// systems consume these after the cooker verifies them.
enum class CookedAssetKind : std::uint8_t {
    MotionClip = 0,        // sampled motion clip (root + joint samples)
    TrajectoryPolicy = 1,  // small numeric controller/trajectory table
    ParameterTable = 2     // balance/tuning parameters for a runtime system
};

// The canonical payload for a motion clip: root positions/orientations plus
// per-joint local rotations sampled at a fixed frame rate, with a skeleton
// size. `cook` rejects (all-or-nothing) clips whose arrays do not line up
// (joints x frames), empty frames, or non-finite samples.
struct ClipPayload {
    std::size_t jointCount{ 0 };
    std::size_t frameCount{ 0 };
    float frameRate{ 0.0f };  // samples per second, > 0
    std::vector<glm::vec3> rootPositions;        // frameCount entries
    std::vector<glm::quat> rootOrientations;     // frameCount entries
    std::vector<glm::quat> jointRotations;       // jointCount * frameCount
};

// The canonical payload for a small trajectory policy: a table of reference
// states the runtime can follow (e.g. desired root velocity / curvature per
// gait phase). Entries are time, root velocity, root turn rate.
struct TrajectoryPayload {
    std::vector<glm::vec3> times;            // seconds, strictly increasing
    std::vector<glm::vec3> rootVelocities;   // same size
    std::vector<float> turnRates;            // same size, radians/s
};

// The canonical payload for a parameter table: named tuning values exported by
// the farm (e.g. gait timings, contact gains). Names must be non-empty, unique,
// and values finite.
struct ParameterPayload {
    std::vector<std::string> names;
    std::vector<float> values;  // same size
};

// A cooked, signed asset as the runtime consumes it. Immutable after cook:
// `signature` covers the kind + payload, so verify() passes only for the exact
// signed bytes.
struct CookedFarmAsset {
    CookedAssetKind kind{ CookedAssetKind::MotionClip };
    std::uint64_t signature{ 0 };
    ClipPayload clip;             // valid when kind == MotionClip
    TrajectoryPayload trajectory;  // valid when kind == TrajectoryPolicy
    ParameterPayload parameters;   // valid when kind == ParameterTable
};

// The offline-farm plugin kinds. The TRAINING tools (MuJoCo / MuJoCo MPC /
// DeepMimic) are OFFLINE-FARM ONLY and are REFUSED by create_farm_cooker with
// a diagnostic — the runtime never links a training environment.
enum class FarmKind : std::uint8_t {
    RuntimeCooker = 0,  // implemented: validate + sign cooked assets
    Mujoco = 1,         // offline training/validation tool (Apache-2.0, reference only)
    MujocoMpc = 2,      // offline predictive-control tool (reference only)
    DeepMimic = 3       // offline imitation-learning tool (reference only)
};

class IFarmCooker {
public:
    virtual ~IFarmCooker() = default;

    virtual FarmKind kind() const noexcept = 0;

    // Cooks + signs an asset from the canonical payloads. Exactly ONE payload
    // family must be set (the one matching `kind`). Refuses (false +
    // diagnostic) empty/invalid payloads, non-finite samples, mismatched array
    // sizes, non-unique/non-increasing tables. On success `out.signature` is
    // the deterministic FNV-1a 64 of the canonical kind+payload bytes.
    virtual bool cook(CookedAssetKind kind, const ClipPayload& clip,
                      const TrajectoryPayload& trajectory,
                      const ParameterPayload& parameters, CookedFarmAsset& out,
                      std::string& errorOut) = 0;

    // Verifies a cooked asset: recomputes the signature over the payload and
    // compares it to `asset.signature`. Returns false if the signature does
    // not match (tampered / corrupt payload) or the asset is structurally
    // invalid for its kind.
    virtual bool verify(const CookedFarmAsset& asset,
                        std::string& errorOut) const = 0;
};

// The seam. RuntimeCooker is implemented (`src/engine/sdk/OfflineFarm.cpp`).
// The TRAINING kinds are REFUSED with a diagnostic — the farm runs offline,
// never inside the game; a refused training backend must never look like a
// working runtime simulator (the deformable/FEMFX/Tressfx plugin pattern).
std::unique_ptr<IFarmCooker> create_farm_cooker(FarmKind kind,
                                                std::string& errorOut);

}  // namespace Engine::Farm
