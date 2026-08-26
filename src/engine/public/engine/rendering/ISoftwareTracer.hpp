#pragma once

// ISoftwareTracer — Agente 1 (task_plan A.7), the PUBLIC software ray tracer
// contract: sphere tracing against signed distance fields / clipmaps / voxel
// SDFs for hardware WITHOUT ray tracing (the honest fallback). The renderer
// binds an SDF built from the surface cache (A.3) or voxel distance fields; the
// tracer marches it deterministically to answer hit/occlusion/normal queries
// that the GI/reflection passes need.
//
// Self-contained (std + glm), no Vulkan, no GPU. Deterministic: the same
// origin/direction/SDF/config reproduce the same hit bit-exactly.

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Rendering {

// A signed distance field: returns the (signed) distance to the nearest
// surface at `p` (negative inside). Must be a pure function of its input.
using DistanceField = std::function<float(const glm::vec3& p)>;

struct SoftwareTraceConfig {
    std::uint32_t maxSteps{ 256 };     // [1, ...] march step cap
    float maxDistance{ 512.0f };       // > 0, ray distance cap
    float hitEpsilon{ 0.01f };         // > 0, surface threshold
    float normalEpsilon{ 0.1f };       // > 0, gradient step for normals
};

struct SoftwareTraceHit {
    bool hit{ false };
    float distance{ 0.0f };            // distance traveled along the ray
    glm::vec3 position{ 0.0f };        // hit point (valid when hit)
    glm::vec3 normal{ 0.0f };          // surface normal (valid when hit)
    std::uint32_t steps{ 0 };          // steps actually taken
};

class ISoftwareTracer {
public:
    virtual ~ISoftwareTracer() = default;

    // Validates/applies the config (all-or-nothing; never clamps).
    virtual bool configure(const SoftwareTraceConfig& config,
                           std::string& errorOut) = 0;
    virtual const SoftwareTraceConfig& config() const noexcept = 0;

    // Sphere-traces a ray against the SDF. `dir` must be unit-length (normalized
    // if not). Returns hit/miss + hit point + analytic normal.
    virtual SoftwareTraceHit trace(const glm::vec3& origin, const glm::vec3& dir,
                                   const DistanceField& sdf) const = 0;

    // Shadow-ray occlusion query: is `to` occluded from `origin` (any surface
    // between them, excluding `to` itself)? Deterministic.
    virtual bool occluded(const glm::vec3& origin, const glm::vec3& to,
                          const DistanceField& sdf) const = 0;

    // Central-difference surface normal at a surface point.
    virtual glm::vec3 surface_normal(const glm::vec3& p,
                                     const DistanceField& sdf) const = 0;
};

// Public factory (defaults config, always succeeds).
std::unique_ptr<ISoftwareTracer> create_software_tracer(std::string& errorOut);

}  // namespace Engine::Rendering
