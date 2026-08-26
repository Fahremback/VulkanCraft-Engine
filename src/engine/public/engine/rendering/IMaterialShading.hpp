#pragma once

// IMaterialShading — Agente 1 (task_plan A.14), the PUBLIC material shading
// core: two-sided foliage shading, simple subsurface (wrap + thickness
// transmission) and "really dark interiors" (ambient decay with depth into an
// enclosed space), with shadows integrated through an injected factor. One
// surface for the deterministic shading MATH a renderer needs, without
// depending on the concrete backend.
//
// SCOPE: the deterministic, headless ALGORITHM of per-surface shading:
//   twoSided        — backfaces of foliage flip the shading normal (the leaf
//                     is lit from behind instead of turning black);
//   wrapLight       — wrapped diffuse (N.L + scatter)/(1 + scatter): scatter 0
//                     is standard clamped N.L, scatter 1 is half-Lambert;
//   subsurface      — transmission through thin geometry:
//                     maxTransmission * exp(-thickness); zero for opaque;
//   interiorAmbient — ambient scale decaying with depth into an interior:
//                     floor + (1 - floor) * exp(-falloff * depth): an enclosed
//                     space with no light keeps only the ambient floor
//                     ("really dark interiors");
//   shadow          — an injected factor [0, 1] from the occlusion seams
//                     (IRayTracer::occluded / ISoftwareTracer / a shadow map):
//                     directDiffuse = wrapLight * (1 - shadow).
// Self-contained (std + glm), bit-exact for the same inputs on every machine.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----

struct MaterialShadingConfig {
    float subsurfaceScatter{ 0.5f };        // wrap amount [0, 1] (0 = none)
    float subsurfaceTransmissionMax{ 0.5f }; // max transmission of thin geometry [0, 1]
    float interiorFalloffPerMeter{ 0.8f };   // ambient decay with interior depth >= 0
    float interiorAmbientFloor{ 0.02f };     // minimum ambient in deep interiors [0, 1)

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan A.14) ----

struct ShadingSurface {
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };  // unit, points out of the surface
    glm::vec3 viewDir{ 0.0f, 0.0f, 1.0f };  // unit, surface -> camera
    bool twoSided{ false };   // foliage: backfaces flip the shading normal
    bool backface{ false };   // this fragment is a backface (dot(n, view) < 0)
    float thickness{ 0.5f };  // meters, for subsurface transmission (>= 0)
    float interiorDepth{ 0.0f };  // meters into an enclosed interior (>= 0)
};

struct ShadingResult {
    glm::vec3 shadingNormal{ 0.0f, 1.0f, 0.0f };  // possibly flipped
    float wrapLight{ 0.0f };       // wrapped N.L in [0, 1]
    float subsurface{ 0.0f };      // transmission through thin geometry [0, 1]
    float interiorAmbient{ 1.0f }; // ambient scale by interior depth [floor, 1]
    float directDiffuse{ 0.0f };   // wrapLight * (1 - shadow) in [0, 1]
    float total{ 0.0f };           // directDiffuse + interiorAmbient (the lit
                                   // fraction; deep shadowed interiors stay low)
};

class IMaterialShading {
public:
    virtual ~IMaterialShading() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const MaterialShadingConfig& config,
                           std::string& errorOut) = 0;
    virtual const MaterialShadingConfig& config() const noexcept = 0;

    // JSON {subsurfaceScatter, subsurfaceTransmissionMax, interiorFalloffPerMeter,
    // interiorAmbientFloor, version:1}. version != 1 or a malformed field
    // refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Shading normal: flips for backfaces when twoSided, else the surface normal.
    virtual glm::vec3 shadingNormal(const ShadingSurface& surface) const noexcept = 0;

    // Wrapped diffuse: (dot(n, l) + scatter) / (1 + scatter), clamped to [0, 1].
    virtual float wrapLight(const glm::vec3& normal,
                            const glm::vec3& lightDir) const noexcept = 0;

    // Transmission through thin geometry: max * exp(-thickness). Zero for
    // opaque materials (max = 0).
    virtual float subsurfaceTransmission(float thickness) const noexcept = 0;

    // Ambient scale by interior depth: floor + (1 - floor) * exp(-falloff * depth).
    virtual float interiorAmbient(float depth) const noexcept = 0;

    // Full evaluation: shading normal, wrap light, subsurface transmission,
    // interior ambient, direct diffuse (wrapLight * (1 - shadow)) and the
    // total lit fraction (directDiffuse + interiorAmbient).
    virtual ShadingResult evaluate(const ShadingSurface& surface,
                                   const glm::vec3& lightDir,
                                   float shadowFactor) const noexcept = 0;
};

// ---- public factory ----

std::unique_ptr<IMaterialShading> create_material_shading(std::string& errorOut);
std::unique_ptr<IMaterialShading> create_material_shading_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering
