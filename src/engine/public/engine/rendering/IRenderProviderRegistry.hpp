#pragma once

// IRenderProviderRegistry — Agente 1 (task_plan H.1/H.2/H.6): the REAL
// per-system provider registry. Every rendering system that can be backed by
// more than one implementation (GI, reflections, ray tracing, software
// tracing, denoiser, water, particles, atmosphere, clouds, materials, post)
// records its chosen provider, the actual call site in the product, the
// runtime/packaging artifact and the capability reason (hardware available /
// fallback / default). The game executable consults this registry at boot and
// logs the complete selection — the honest, data-driven answer to "which
// vendor actually runs in this build and where is it consumed" (docs
// SOLUCOES_E_DEPENDENCIAS matrix mirror, kept in sync with the real wiring).
//
// Self-contained (std only). Deterministic: the same set() sequence produces
// the same registry, and set() replaces the previous entry of a system
// (last-write-wins — the product records its FINAL selection).

#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

struct RenderProviderEntry {
    std::string system;      // stable system key, e.g. "gi", "reflections",
                             // "rayTracer", "screenTracing", "denoiser",
                             // "water", "particles", "atmosphere", "clouds",
                             // "materials", "surfaceCache", "toneMapping",
                             // "post"
    std::string provider;    // selected implementation, e.g. "radiance-cache+restir",
                             // "embree", "vulkan-ray-query", "ddgi-probe-grid"
    std::string callSite;    // real consumption point in the product, e.g.
                             // "draw(): lumenRayTracer->closestHit() over surface cards"
    std::string artifact;    // runtime/packaging artifact, e.g. "vc_sdk_rendering",
                             // "shaders/voxel.frag", "assets/effects/block_simple.efk"
    std::string capability;  // why this provider was chosen: "device-has-rt",
                             // "fallback", "default", "capability-gated"
};

class IRenderProviderRegistry {
public:
    virtual ~IRenderProviderRegistry() = default;

    // Records one provider selection (replaces any previous entry for `system`).
    virtual void set(const RenderProviderEntry& entry) = 0;

    // Returns the recorded entry for a system (nullptr when not recorded).
    virtual const RenderProviderEntry* find(const std::string& system) const = 0;

    // All recorded entries, in insertion order.
    virtual std::vector<RenderProviderEntry> all() const = 0;

    // Deterministic JSON: {"systems":[{...}, ...]}.
    virtual std::string to_json() const = 0;

    virtual void clear() = 0;
};

// Public factory (always succeeds).
std::unique_ptr<IRenderProviderRegistry> create_render_provider_registry(
    std::string& errorOut);

}  // namespace Engine::Rendering
