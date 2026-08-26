#pragma once

// IDiffuseGlobalIllumination — Agente 1 (task_plan A.5), the PUBLIC dynamic
// diffuse GI contract: the multi-bounce radiosity pass that turns the captured
// material cards (A.4) into per-card DIRECT + INDIRECT irradiance. It is the
// Lumen "diffuse GI with color bleeding" analogue, expressed as a bounded,
// deterministic Jacobi radiosity iteration:
//
//   direct_i   = irradiance_i + skylight * skyVisibility_i   (shadowed skylight)
//   radiosity_i = emissive_i + albedo_i * (direct_i + gathered_i)
//   gathered_i = sum_j formFactor(i, j) * radiosity_j          (one gather per bounce)
//
// Emissive cards are light SOURCES (their emissive term is added every bounce,
// never attenuated by their own albedo); every card's outgoing light is its
// albedo * (direct + gathered), so a red surface bleeds red into its neighbors.
// The skylight term is shadowed by the card's captured sky visibility (A.4).
//
// The form factor is the standard diffuse-to-diffuse kernel
// (cos_i * cos_j / (pi * d^2)) * area_j, clamped to [0, 1], with a maxDistance
// cull so the pass is O(n^2) over the surviving pairs (bounded, deterministic).
//
// Self-contained (std + glm), no Vulkan. Deterministic: the same cards + config
// reproduce the same per-card direct/indirect bit-exactly.

#include "engine/rendering/ISurfaceCacheCapture.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// Per-card result of the multi-bounce solve.
struct DiffuseGiResult {
    glm::vec3 direct{ 0.0f };    // irradiance + shadowed skylight
    glm::vec3 indirect{ 0.0f };  // gathered multi-bounce radiance
    glm::vec3 outgoing{ 0.0f };  // emissive + albedo * (direct + indirect)
};

struct DiffuseGiConfig {
    std::uint32_t bounces{ 2 };       // [1, 8] gather iterations
    glm::vec3 skylight{ 0.05f, 0.07f, 0.10f };  // shadowed ambient term
    float maxDistance{ 128.0f };      // > 0, form-factor cull distance
    float intensity{ 1.0f };          // [0.01, 64] global scale on gathered light
};

class IDiffuseGlobalIllumination {
public:
    virtual ~IDiffuseGlobalIllumination() = default;

    // Validates/applies the config (all-or-nothing; never clamps).
    virtual bool configure(const DiffuseGiConfig& config, std::string& errorOut) = 0;
    virtual const DiffuseGiConfig& config() const noexcept = 0;

    // Binds the captured cards to illuminate (all-or-nothing on validation).
    virtual bool set_cards(const std::vector<CapturedCard>& cards,
                           std::string& errorOut) = 0;

    // Runs the multi-bounce radiosity solve. Returns false on invalid state.
    virtual bool solve(std::string& errorOut) = 0;

    virtual std::uint32_t card_count() const noexcept = 0;
    virtual bool result(std::uint32_t index, DiffuseGiResult& out) const = 0;

    // Total gathered energy injected at a given bounce (1-based), for
    // debug/validation of convergence.
    virtual float bounce_energy(std::uint32_t bounce) const = 0;
};

// Public factory (defaults config, always succeeds).
std::unique_ptr<IDiffuseGlobalIllumination> create_diffuse_global_illumination(
    std::string& errorOut);

}  // namespace Engine::Rendering
