#pragma once

// IScreenSpaceTracer — Agente 1 (task_plan A.6), the PUBLIC screen-space trace
// contract: GI/reflection screen traces with HISTORY, REPROJECTION,
// DISOCCLUSION and off-screen FALLBACK. This is the pure, deterministic
// screen-space ray march against a depth buffer — the "software screen trace"
// the renderer runs before falling back to probes / ray tracing (task A.6).
//
// The depth buffer is injected as a SAMPLER (a pure function of UV -> linear
// depth), so the contract is headless: tests bind a synthetic depth field (a
// plane, a box), the renderer binds the real HZB/depth texture. Reprojection is
// expressed via view-projection matrices (glm::mat4) so it stays device-free.
//
// Self-contained (std + glm), no Vulkan. Deterministic: the same depth field,
// ray and matrices reproduce the same hit / reprojected UV bit-exactly.

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Rendering {

// Linear depth sampler: returns the view-space distance (>= 0) of the nearest
// surface at a screen UV (u,v in [0,1], origin top-left). Must be a pure
// function of the UV.
using DepthSampler = std::function<float(const glm::vec2& uv)>;

struct ScreenTraceConfig {
    std::uint32_t maxSteps{ 128 };   // [1, ...] march step cap
    float stepSize{ 0.05f };         // > 0, initial march step (view-space units)
    float depthBias{ 0.02f };        // > 0, "behind surface" tolerance
    float refineSteps{ 4 };          // binary-refine iterations on crossing
    std::uint32_t viewportWidth{ 1920 };  // [1, ...] for off-screen test
    std::uint32_t viewportHeight{ 1080 };
};

struct ScreenTraceHit {
    bool hit{ false };
    bool offscreen{ false };         // the ray left the viewport (fallback trigger)
    float t{ 0.0f };                 // distance along the ray to the hit
    glm::vec2 uv{ 0.0f };            // hit screen UV (0..1)
    glm::vec3 position{ 0.0f };      // hit position (view space)
    std::uint32_t steps{ 0 };
};

class IScreenSpaceTracer {
public:
    virtual ~IScreenSpaceTracer() = default;

    // Validates/applies the config (all-or-nothing; never clamps).
    virtual bool configure(const ScreenTraceConfig& config,
                           std::string& errorOut) = 0;
    virtual const ScreenTraceConfig& config() const noexcept = 0;

    // Screen-space ray march: traces a VIEW-SPACE ray (origin + unit direction,
    // camera looks down -Z) against `depth`. The ray is projected to screen UV
    // each step; a ray leaving [0,1]^2 sets `offscreen` (the caller falls back
    // to probes / RT). Returns the hit (if any) with UV + t + view position.
    virtual ScreenTraceHit trace(const glm::vec3& viewOrigin,
                                 const glm::vec3& viewDir,
                                 const DepthSampler& depth) const = 0;

    // REPROJECTION: a point in view space `p` is projected through the previous
    // frame's view-projection into the CURRENT frame's screen UV. Used to carry
    // the history buffer. Returns the current-frame UV + linear depth, and
    // whether the point is still in front of the camera / inside the viewport.
    struct ReprojectResult {
        bool valid{ false };
        glm::vec2 uv{ 0.0f };
        float depth{ 0.0f };
    };
    virtual ReprojectResult reproject(const glm::vec3& viewPoint,
                                      const glm::mat4& prevViewProjection,
                                      const glm::mat4& currViewProjection) const = 0;

    // DISOCCLUSION: a history sample is discarded when the surface it came from
    // is no longer visible — i.e. the reprojected depth differs from the depth
    // now present at that pixel by more than `threshold`. Returns true when the
    // history is stale (disoccluded).
    virtual bool disoccluded(float reprojectedDepth, float currentDepth,
                             float threshold) const = 0;
};

// Public factory (defaults config, always succeeds).
std::unique_ptr<IScreenSpaceTracer> create_screen_space_tracer(
    std::string& errorOut);

}  // namespace Engine::Rendering
