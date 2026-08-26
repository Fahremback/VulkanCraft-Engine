#pragma once

// ISurfaceCacheCapture — Agente 1 (task_plan A.4), the PUBLIC Lumen-style
// MATERIAL CARD capture contract. It binds the surface cache (ILumenScene,
// A.3) to a radiance source (the GI core, A.2) and CAPTURES the lighting into
// each surface card: the irradiance sampled at the card, plus the bounced
// radiance (albedo * irradiance) that feeds the multi-bounce / color-bleed
// passes. Emissive cards are flagged self-luminous (their emissive term is the
// radiance; they do not consume a GI sample).
//
// Three behaviors the contract pins down (task_plan A.4):
//   * CAPTURE: a per-frame CARD budget limits how many pending cards are
//     (re)captured each update; pending cards are visited nearest-camera first
//     (the Lumen "capture near first" priority). Deterministic.
//   * LOCALIZED INVALIDATION: `invalidate_chunk(id)` re-queues ONLY that chunk's
//     cards; other chunks keep their captured radiance. A chunk whose surface
//     cards were replaced (revision bump) is re-captured automatically.
//   * VRAM BUDGET: captured cards have a fixed, documented GPU payload, so
//     `vram_bytes()` is an exact budget counter (never an estimate).
//
// Self-contained (std + glm), no Vulkan. Deterministic: the same scene +
// radiance sampler + camera sequence reproduces the same captured radiance
// bit-exactly.

#include "engine/rendering/ILumenScene.hpp"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Rendering {

// The radiance source: returns the diffuse irradiance (RGB) + sky visibility
// (A) at a world position, given the surface normal. The renderer binds the
// real GI core; tests bind a synthetic deterministic sampler. Must be a pure
// function of its inputs (determinism).
using RadianceSampler =
    std::function<glm::vec4(const glm::vec3& position, const glm::vec3& normal)>;

// A captured material card: the surface card plus its captured lighting.
struct CapturedCard {
    LumenSurfaceCard card;            // the surface card that was captured
    glm::vec4 irradiance{ 0.0f };     // sampled diffuse irradiance (RGB) + sky vis (A)
    glm::vec3 bouncedRadiance{ 0.0f };// albedo.rgb * irradiance.rgb (color-bleed source)
    bool selfLuminous{ false };       // emissive card (radiance == emissive)
    std::uint64_t captureAge{ 0 };    // monotonic capture sequence number
};

struct CaptureConfig {
    std::uint32_t cardsPerFrame{ 64 };      // [1, ...] capture budget per update
    std::uint32_t maxCapturedCards{ 4096 }; // [1, ...] captured-card cap (VRAM bound)
};

class ISurfaceCacheCapture {
public:
    virtual ~ISurfaceCacheCapture() = default;

    // Validates/applies the config (all-or-nothing; never clamps).
    virtual bool configure(const CaptureConfig& config, std::string& errorOut) = 0;
    virtual const CaptureConfig& config() const noexcept = 0;

    // Binds the surface cache and the radiance source. Rebinding a scene drops
    // captured state (the new scene is authoritative).
    virtual void bind_scene(ILumenScene* scene) = 0;
    virtual void bind_radiance(RadianceSampler sampler) = 0;

    // LOCALIZED invalidation: re-queues only this chunk's cards (no-op when the
    // chunk is unknown). Returns true if the chunk existed.
    virtual bool invalidate_chunk(std::uint64_t chunkId) = 0;
    // Re-queues every card of every chunk.
    virtual void invalidate_all() = 0;

    // Captures up to cardsPerFrame pending cards, nearest-camera chunks first.
    // Returns how many cards were captured this frame. Automatically (re)captures
    // chunks whose surface cards changed (revision bump) or were invalidated.
    virtual std::uint32_t update(const glm::vec3& cameraPosition) = 0;

    virtual std::uint32_t captured_count() const noexcept = 0;
    virtual std::uint32_t pending_count() const noexcept = 0;
    virtual bool captured(std::uint32_t index, CapturedCard& out) const = 0;

    // Exact VRAM budget (captured cards have a fixed payload). Deterministic.
    virtual std::uint64_t vram_bytes() const noexcept = 0;
    virtual std::uint64_t capture_age() const noexcept = 0;
};

// Public factory (defaults config, always succeeds).
std::unique_ptr<ISurfaceCacheCapture> create_surface_cache_capture(
    std::string& errorOut);

// The fixed GPU payload of one captured card, in bytes (documented ABI).
constexpr std::uint64_t kCapturedCardVramBytes = 64;

}  // namespace Engine::Rendering
