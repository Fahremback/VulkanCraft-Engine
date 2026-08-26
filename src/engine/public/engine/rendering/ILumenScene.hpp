#pragma once

// ILumenScene — Agente 1 (task_plan A.3), the PUBLIC Lumen-style scene
// contract: the SURFACE CACHE the GI/reflection passes trace against, built
// incrementally from the voxel chunks/meshes (handoff 3->1: snapshots / dirty
// chunks / light). One surface for the renderer, the editor debug views, the
// profiler and scripting/CLI/MCP to feed and observe the cache without
// depending on the concrete voxel mesher.
//
// The scene is a flat set of oriented surface CARDs (the Lumen analogue of
// "material cards"): each card carries a world-space center, a unit normal, a
// half extent (the tangent-plane footprint), a linear albedo and an emissive
// term. Cards are grouped by the chunk that produced them so a DIRTY chunk can
// be replaced/removed INCREMENTALLY (no global rebuild) — task_plan A.3's
// "atualização incremental por chunk sujo".
//
// HIERARCHICAL CLIPMAPS / DISTANT REPRESENTATION: `update(camera)` assigns each
// card to a distance CASCADE (band) from the camera. The nearest band is the
// full-resolution surface cache; farther bands are the coarse distant
// representation the software tracer can consult without touching the fine
// cache. The assignment is a pure function of (card center, camera, config),
// so it is deterministic and reproducible.
//
// Self-contained (std + glm), no Vulkan, no voxel types. Deterministic: the
// same (config, chunk-id/revision sequence, surfaces, camera) reproduces the
// same cards and cascades bit-exactly on every machine.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// A raw surface patch fed by the renderer's mesh->surface pass (or a test).
// These are aggregated into cards by `build_cards` (the carding rule is shared
// between the adapter and the tests so both observe the exact same merge).
struct LumenSurface {
    glm::vec3 center{ 0.0f };            // world-space patch center
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f }; // unit normal (normalized on input)
    glm::vec2 halfExtent{ 1.0f };        // tangent-plane footprint (>= 0)
    glm::vec4 albedo{ 0.5f };            // linear diffuse albedo
    glm::vec3 emissive{ 0.0f };          // linear emissive radiance
};

// A cached surface card. Identical to a surface plus bookkeeping: the owning
// chunk (id + revision) and the distance cascade assigned by update().
struct LumenSurfaceCard {
    glm::vec3 center{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    glm::vec2 halfExtent{ 1.0f };
    glm::vec4 albedo{ 0.5f };
    glm::vec3 emissive{ 0.0f };
    std::uint64_t chunkId{ 0 };
    std::uint64_t revision{ 0 };
    std::uint8_t cascade{ 0 };           // 0 = nearest band, up to cascadeCount-1
};

// The pure cache config, validated all-or-nothing (never clamped).
struct LumenSceneConfig {
    std::uint32_t maxCards{ 4096 };         // [1, 65536] global card budget
    std::uint32_t maxCardsPerChunk{ 256 };  // [1, maxCards] per-chunk card budget
    float coplanarDot{ 0.95f };             // [0.5, 1] merge threshold (normal dot)
    float mergeDistance{ 2.0f };            // >= 0 world units, merge window
    std::uint32_t cascadeCount{ 4 };        // [1, 8] distance bands
    float cascadeDistance{ 64.0f };         // > 0 base band distance (doubles per band)
};

// The deterministic, headless surface cache (task_plan A.3).
class ILumenScene {
public:
    virtual ~ILumenScene() = default;

    // Validates/applies the config (all-or-nothing; out-of-range refused).
    virtual bool configure(const LumenSceneConfig& config,
                           std::string& errorOut) = 0;
    virtual const LumenSceneConfig& config() const noexcept = 0;

    // INCREMENTAL update by dirty chunk: atomically replaces the chunk's cards
    // with the cards derived from `surfaces` (via the shared `build_cards`
    // rule). A `revision` <= the stored revision for that chunk is REFUSED as
    // stale (monotonic dirty-chunk updates). All-or-nothing on validation:
    // a malformed surface refuses the whole replacement, leaving the chunk
    // (and every other chunk) untouched.
    virtual bool replace_chunk(std::uint64_t chunkId, std::uint64_t revision,
                               const std::vector<LumenSurface>& surfaces,
                               std::string& errorOut) = 0;

    // Removes a chunk's cards (no-op if unknown). Returns true if the chunk
    // existed.
    virtual bool remove_chunk(std::uint64_t chunkId) = 0;

    virtual std::uint32_t card_count() const noexcept = 0;
    virtual bool card(std::uint32_t index, LumenSurfaceCard& out) const = 0;

    // Assigns each card to a distance cascade from the camera (hierarchical
    // distant representation). Pure + deterministic.
    virtual void update(const glm::vec3& cameraPosition) = 0;

    // Chunk bookkeeping for the dirty-chunk protocol.
    virtual bool has_chunk(std::uint64_t chunkId) const noexcept = 0;
    virtual std::uint64_t chunk_revision(std::uint64_t chunkId) const noexcept = 0;
    virtual std::uint32_t chunk_card_count(std::uint64_t chunkId) const noexcept = 0;

    // Debug/profiler: card counts per cascade after the last update().
    virtual std::uint32_t cards_in_cascade(std::uint8_t cascade) const = 0;
};

// Public factory (defaults config, always succeeds).
std::unique_ptr<ILumenScene> create_lumen_scene(std::string& errorOut);

// The shared, deterministic carding rule: aggregates the surfaces of ONE chunk
// into surface cards. Coplanar surfaces within mergeDistance (and with close
// albedo) merge into one card whose tangent-plane extent grows to the union;
// the result is emitted in a stable (sorted) order. Exposed so the renderer's
// mesh->surface pass and the tests use the exact same merge.
std::vector<LumenSurfaceCard> build_cards(
    std::uint64_t chunkId, std::uint64_t revision,
    const std::vector<LumenSurface>& surfaces, const LumenSceneConfig& config);

}  // namespace Engine::Rendering
