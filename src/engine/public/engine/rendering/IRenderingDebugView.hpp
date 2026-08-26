#pragma once

// IRenderingDebugView — Agente 1 (task_plan A.17), the PUBLIC debug-view DATA
// MODEL: a serializable snapshot of every headless rendering contract, so the
// editor/profiler/CLI/MCP can render the debug views (LumenScene, cards,
// probes, reservoirs, tracing path, disocclusion, denoiser confidence) without
// touching the concrete backends. The GPU-side visualizer consumes this model;
// the model itself is pure and headless.
//
// Self-contained (std + glm). Deterministic: the same contract state serializes
// to the same JSON bit-exactly.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// One surface-card entry in the debug snapshot (LumenScene view).
struct DebugCard {
    glm::vec3 center{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    glm::vec4 albedo{ 0.5f };
    glm::vec3 emissive{ 0.0f };
    std::uint8_t cascade{ 0 };
};

// One probe entry (probes view).
struct DebugProbe {
    glm::vec4 radianceVisibility{ 0.0f };
    glm::ivec4 worldCellCascade{ 0, 0, 0, -1 };
};

// A recorded trace path (tracing-path view).
struct DebugTracePath {
    glm::vec3 origin{ 0.0f };
    glm::vec3 direction{ 0.0f };
    bool hit{ false };
    float distance{ 0.0f };
    std::uint32_t steps{ 0 };
};

struct RenderingDebugSnapshot {
    // LumenScene / cards view.
    std::uint32_t cardCount{ 0 };
    std::vector<DebugCard> cards;
    std::vector<std::uint32_t> cardsPerCascade;

    // Capture view (A.4).
    std::uint32_t capturedCount{ 0 };
    std::uint32_t pendingCount{ 0 };
    std::uint64_t vramBytes{ 0 };

    // Probes view (A.2).
    std::uint32_t probeCount{ 0 };
    std::uint32_t pendingProbes{ 0 };
    std::uint32_t sunRevision{ 0 };
    std::vector<DebugProbe> probes;

    // Tracing-path / disocclusion / denoiser-confidence views (A.6/A.7).
    std::vector<DebugTracePath> tracePaths;
    std::uint32_t disoccludedPixels{ 0 };
    std::uint32_t confidenceLevel{ 0 };  // 0..3 aggregate denoiser confidence
};

class IRenderingDebugView {
public:
    virtual ~IRenderingDebugView() = default;

    // Rebuilds the snapshot from the bound state (see bind_* below).
    virtual void refresh() = 0;
    virtual const RenderingDebugSnapshot& snapshot() const noexcept = 0;

    // Serializes the current snapshot to canonical JSON (deterministic key
    // order + number formatting).
    virtual std::string to_json() const = 0;

    // Bindable state seams — each is optional; unbound state reads as empty.
    virtual void bind_cards(const std::vector<DebugCard>& cards,
                            const std::vector<std::uint32_t>& cardsPerCascade) = 0;
    virtual void bind_capture(std::uint32_t captured, std::uint32_t pending,
                              std::uint64_t vramBytes) = 0;
    virtual void bind_probes(const std::vector<DebugProbe>& probes,
                             std::uint32_t pending, std::uint32_t sunRevision) = 0;
    virtual void add_trace_path(const DebugTracePath& path) = 0;
    virtual void bind_disocclusion(std::uint32_t pixels, std::uint32_t confidence) = 0;
};

// Public factory (always succeeds).
std::unique_ptr<IRenderingDebugView> create_rendering_debug_view(
    std::string& errorOut);

}  // namespace Engine::Rendering
