#pragma once

// IRenderPassMetrics — Agente 1 (task_plan B): the PUBLIC producer-side
// contract for deep rendering telemetry — per-pass CPU/GPU timing, memory
// pools and streaming counters — consumed by the editor profiler (Agente 2's
// IFrameProfiler is the frame-level consumer; this is the renderer-side
// producer that feeds it per-pass data). The Vulkan seam only SAMPLES the
// clocks/counters; this surface is the deterministic aggregator (window,
// percentiles, JSON) — pure CPU, fully headless-testable.
//
// Deterministic: same recorded sequence → same snapshot/JSON (p95/p99 via
// sorted samples; ties by pass order of first appearance). All-or-nothing:
// negative/NaN durations, unknown pass reset, unknown pool reset and unknown
// stream reset are refused without mutating state.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

inline constexpr std::size_t kRenderMetricsDefaultWindow = 600;  // 10s @ 60fps

// Per-pass aggregate over the sliding window.
struct RenderPassStats {
    std::string name;
    std::size_t samples{ 0 };
    double cpuMsMin{ 0.0 };
    double cpuMsMax{ 0.0 };
    double cpuMsAvg{ 0.0 };
    double cpuMsP95{ 0.0 };
    double cpuMsP99{ 0.0 };
    double gpuMsMin{ 0.0 };
    double gpuMsMax{ 0.0 };
    double gpuMsAvg{ 0.0 };
    double gpuMsP95{ 0.0 };
    double gpuMsP99{ 0.0 };
};

// Memory pool telemetry (VRAM or heap; caller picks the unit).
struct RenderMemoryPoolStats {
    std::string name;
    std::size_t samples{ 0 };
    std::uint64_t currentBytes{ 0 };
    std::uint64_t peakBytes{ 0 };
    std::uint64_t minBytes{ 0 };
    std::uint64_t maxBytes{ 0 };
};

// Streaming counters (chunks/cards/etc. loaded/evicted per frame).
struct RenderStreamingStats {
    std::string name;
    std::size_t frames{ 0 };
    std::uint64_t totalLoaded{ 0 };
    std::uint64_t totalEvicted{ 0 };
    std::uint64_t bytesLoaded{ 0 };
    std::uint64_t peakBytesInFlight{ 0 };
};

// Immutable snapshot of the whole renderer telemetry.
struct RenderMetricsSnapshot {
    std::vector<RenderPassStats> passes;          // pass order = first appearance
    std::vector<RenderMemoryPoolStats> pools;     // pool order = first appearance
    std::vector<RenderStreamingStats> streams;    // stream order = first appearance
};

class IRenderPassMetrics {
public:
    virtual ~IRenderPassMetrics() = default;

    // Records one frame of per-pass timing. cpuMs/gpuMs >= 0; NaN/negative
    // refused (return false, no mutation). The window slides past capacity.
    virtual bool recordPass(const std::string& pass, double cpuMs, double gpuMs) = 0;

    // Samples a memory pool's residency. Unknown pool (first time) is created.
    // bytes >= 0; refused otherwise (no mutation).
    virtual bool recordMemory(const std::string& pool, std::uint64_t bytes) = 0;

    // Records streaming activity for one frame. Unknown stream created.
    // Any negative delta refused (no mutation). bytesLoaded >= 0.
    virtual bool recordStreaming(const std::string& stream,
                                 std::uint64_t loaded, std::uint64_t evicted,
                                 std::uint64_t bytesLoaded) = 0;

    // Closes the current frame: finalized streaming stats are snapshotted and
    // per-frame accumulators reset. Pass/memory windows keep sliding per record.
    virtual void endFrame() = 0;

    // Clears everything (windows, pools, streams, counters).
    virtual void clear() = 0;

    // Deterministic snapshot.
    virtual RenderMetricsSnapshot snapshot() const = 0;

    // Deterministic JSON snapshot (%.6g; key order fixed).
    virtual std::string to_json() const = 0;
};

// Creates the producer with a sliding window of `capacity` frames per pass.
// capacity == 0 -> default (kRenderMetricsDefaultWindow). Never null.
std::unique_ptr<IRenderPassMetrics> create_render_pass_metrics(std::size_t capacity);

}  // namespace Engine::Rendering
