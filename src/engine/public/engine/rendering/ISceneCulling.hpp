#pragma once

// ISceneCulling — Agente 1 (task_plan B.8), the PUBLIC scene culling core:
// frustum culling, distance LOD with hysteresis, conservative occlusion and
// instance grouping. One surface for the culling MATH of the renderer
// (instancing/meshlets/LOD/culling/occlusion) without depending on the
// concrete backend.
//
// SCOPE: the deterministic, headless ALGORITHM of scene culling:
//   frustum     — Gribb-Hartmann plane extraction from a view-projection
//                 matrix; AABB (p-vertex test) and sphere (signed distance)
//                 visibility;
//   lod         — distance thresholds in powers of two (lod0Distance,
//                 2x, 4x, ...) with a HYSTERESIS band: switching to a finer
//                 level requires crossing the enter threshold, back to a
//                 coarser level the (larger) leave threshold — no popping;
//   occlusion   — conservative screen-space test: the candidate's projected
//                 rect fully inside the occluder's rect AND the candidate's
//                 nearest view depth beyond the occluder's farthest depth;
//   instancing  — grouping of instances by (mesh, material) with combined
//                 AABBs and per-group counts (the meshlet/instance stream
//                 input of the renderer).
// Self-contained (std + glm), bit-exact for the same inputs on every machine.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----

struct SceneCullingConfig {
    float lod0Distance{ 32.0f };    // LOD 0 -> 1 boundary in meters (0, inf)
    float lodHysteresis{ 0.15f };   // relative hysteresis band [0, 0.5]
    std::uint32_t maxInstances{ 1024 };  // instance stream limit [1, 1<<20]

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan B.8) ----

struct Frustum {
    // Six planes in the form a*x + b*y + c*z + d (unit normals), order:
    // left, right, top, bottom, near, far.
    glm::vec4 planes[6];
};

struct SceneInstance {
    glm::vec3 position{ 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f, 1.0f, 1.0f };
    std::uint32_t mesh{ 0 };
    std::uint32_t material{ 0 };
};

struct InstanceGroup {
    std::uint32_t mesh{ 0 };
    std::uint32_t material{ 0 };
    std::uint32_t count{ 0 };         // instances merged into this group
    glm::vec3 aabbMin{ 0.0f, 0.0f, 0.0f };
    glm::vec3 aabbMax{ 0.0f, 0.0f, 0.0f };
};

class ISceneCulling {
public:
    virtual ~ISceneCulling() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const SceneCullingConfig& config,
                           std::string& errorOut) = 0;
    virtual const SceneCullingConfig& config() const noexcept = 0;

    // JSON {lod0Distance, lodHysteresis, maxInstances, version:1}. version !=
    // 1 or a malformed field refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Gribb-Hartmann: six unit-normal planes from the view-projection matrix.
    virtual Frustum extractFrustum(const glm::mat4& viewProj) const noexcept = 0;

    // Visibility tests (conservative: an object touching the frustum is in).
    virtual bool sphereVisible(const Frustum& frustum, const glm::vec3& center,
                               float radius) const noexcept = 0;
    virtual bool aabbVisible(const Frustum& frustum, const glm::vec3& min,
                             const glm::vec3& max) const noexcept = 0;

    // Distance LOD: 0 at [0, lod0), 1 at [lod0, 2*lod0), 2 at [2*lod0,
    // 4*lod0), 3 beyond. Hysteretic form keeps the current level until the
    // distance crosses the LEAVE threshold (coarser) or the ENTER threshold
    // (finer), so the boundary does not pop.
    virtual std::uint32_t selectLod(float distance) const noexcept = 0;
    virtual std::uint32_t selectLodHysteretic(
        float distance, std::uint32_t currentLod) const noexcept = 0;

    // Conservative occlusion: candidate fully behind an occluder from the
    // camera. Both AABBs projected with `viewProj`; occluded when the
    // candidate's screen rect is inside the occluder's rect and the
    // candidate's nearest view depth is beyond the occluder's farthest.
    virtual bool occluded(const glm::mat4& viewProj, const glm::vec3& occMin,
                          const glm::vec3& occMax, const glm::vec3& candMin,
                          const glm::vec3& candMax) const noexcept = 0;

    // Instance grouping: merges instances with the same (mesh, material) into
    // one group with count and combined AABB. Refuses all-or-nothing when the
    // stream exceeds maxInstances (output untouched).
    virtual bool buildInstanceGroups(
        const std::vector<SceneInstance>& instances,
        std::vector<InstanceGroup>& groups, std::string& errorOut) const = 0;
};

// ---- public factory ----

std::unique_ptr<ISceneCulling> create_scene_culling(std::string& errorOut);
std::unique_ptr<ISceneCulling> create_scene_culling_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering
