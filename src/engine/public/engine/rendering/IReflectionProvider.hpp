#pragma once

// IReflectionProvider — Agente 1 (task_plan A.1), the PUBLIC reflection
// contract: screen-space, probe and ray-traced reflections behind ONE surface.
// The renderer asks the provider for the per-surface reflection mode and
// drives the budget; the editor/profiler observe the mode and the per-frame
// cost without depending on the concrete backend.
//
// PLUGIN ARCHITECTURE: backends are OPTIONAL, selected DATA-DRIVEN + capability
// check, and a requested backend that is not available is REFUSED with a
// diagnostic (never silently falls back) — the capability is the source of
// truth, mirroring IGlobalIlluminationProvider / IHairProvider.
//
// Self-contained (std + glm). Deterministic: a mode decision is a pure
// function of (material params, capability, config) with no RNG.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Rendering {

// Reflection backends, from cheapest to most expensive.
enum class ReflectionBackend : std::uint8_t {
    None,            // no reflections (rough surfaces only)
    ScreenSpace,     // screen traces with history/reprojection (software fallback)
    Probe,           // radiance-cache / DDGI probes (low-frequency)
    RayTraced,       // hardware ray tracing when available

    Count
};

struct ReflectionCapabilities {
    bool screenSpace{ true };  // software screen traces (always available)
    bool probe{ false };       // a radiance cache is bound
    bool rayTraced{ false };   // hardware ray tracing present
};

// Per-surface inputs that decide the reflection mode. Named distinctly from
// IReflectionModel::ReflectionSurface (the richer per-surface shading inputs)
// so both public headers can be included in one TU without a type collision.
struct ReflectionSurfaceInput {
    float roughness{ 0.5f };  // [0, 1]
    float clearCoat{ 0.0f };  // [0, 1] — clear-coat layer
    float metalness{ 0.0f };  // [0, 1]
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };  // world-space, normalized
};

struct ReflectionConfig {
    // Screen-trace budget for the whole frame.
    std::uint32_t maxScreenRays{ 4096 };
    // Minimum roughness that still receives screen-space reflections.
    float screenRoughnessLimit{ 0.45f };
    // Probe reflections cover everything above this roughness.
    float probeRoughnessFloor{ 0.45f };
};

class IReflectionProvider {
public:
    virtual ~IReflectionProvider() = default;

    virtual ReflectionBackend backend() const noexcept = 0;
    virtual ReflectionCapabilities capabilities() const noexcept = 0;

    // Validates/applies the config (all-or-nothing, never clamps).
    virtual bool configure(const ReflectionConfig& config,
                           std::string& errorOut) = 0;
    virtual const ReflectionConfig& config() const noexcept = 0;

    // Decides the mode for one surface. Deterministic: a pure function of the
    // material inputs, the capability and the config. Always falls back to
    // None when no backend is available; never claims an unavailable backend.
    virtual ReflectionBackend resolve_mode(const ReflectionSurfaceInput& surface) const = 0;

    // Debug view + profiler: the last frame's mode distribution (count of
    // surfaces per backend) and screen-ray spend.
    virtual std::uint32_t surfaces_in_mode(ReflectionBackend mode) const noexcept = 0;
    virtual std::uint32_t screen_rays_used() const noexcept = 0;
};

// The plugin seam: ScreenSpace is implemented headlessly (the mode decision +
// budget accounting); Probe/RayTraced are REFUSED with a diagnostic when the
// requested capability is not present (a missing plugin/device is never
// mistaken for working reflections).
std::unique_ptr<IReflectionProvider> create_reflection_provider(
    ReflectionBackend backend, const ReflectionCapabilities& capabilities,
    std::string& errorOut);

}  // namespace Engine::Rendering