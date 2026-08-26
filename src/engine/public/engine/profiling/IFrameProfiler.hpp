#pragma once

// IFrameProfiler (agente 2 §B): the PUBLIC headless frame-profiler contract.
// The editor's profiler window today is a raw ImGui graph fed by
// set_frame_stats(fps, ms) with NO deterministic statistics — this contract
// makes frame/memory telemetry data-driven and bit-exact testable:
//   - SLIDING WINDOW: record(frameMs, heapMb) per frame; the window keeps the
//     last N samples (ring, deterministic order — oldest dropped first).
//   - STATISTICS: min / max / avg / p95 / p99 over the window, plus
//     spike_count (frames above a configurable threshold), fps (1000/frameMs
//     of the LAST sample) and the current heap peak.
//   - PERCENTILES: p95/p99 computed from the sorted window (nearest-rank).
//     Empty window -> avg/p95/p99 = 0, min/max = 0, fps = 0 (never NaN).
//   - DETERMINISM: no clocks, no RNG, no globals; time only enters via
//     record(frameMs). Same sequence of records -> identical snapshot.
//   - RESET: clear() empties the window and zeroes stats (used on mode
//     changes: entering/exiting Play, switching scenes).
//   - OBSERVABLE: to_json() serializes {samples,min,max,avg,p95,p99,
//     spike_count,fps,heap_mb} deterministically (editor exposes it via the
//     Control API, e.g. GET /profiler).
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/FrameProfiler.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace profiling {

// The frame-telemetry snapshot (observable via the editor's Control API).
struct ProfilerSnapshot {
    std::size_t samples{ 0 };       // frames in the window
    double minMs{ 0.0 };
    double maxMs{ 0.0 };
    double avgMs{ 0.0 };
    double p95Ms{ 0.0 };
    double p99Ms{ 0.0 };
    std::uint64_t spikeCount{ 0 };  // frames above the spike threshold
    double fps{ 0.0 };              // from the LAST recorded frame
    double heapMb{ 0.0 };           // heap of the LAST recorded frame
    double heapPeakMb{ 0.0 };       // max heap seen since reset

    bool operator==(const ProfilerSnapshot& other) const {
        return samples == other.samples && minMs == other.minMs &&
               maxMs == other.maxMs && avgMs == other.avgMs &&
               p95Ms == other.p95Ms && p99Ms == other.p99Ms &&
               spikeCount == other.spikeCount && fps == other.fps &&
               heapMb == other.heapMb && heapPeakMb == other.heapPeakMb;
    }
    bool operator!=(const ProfilerSnapshot& other) const {
        return !(*this == other);
    }
};

class IFrameProfiler {
public:
    virtual ~IFrameProfiler() = default;

    // Records one frame (frameMs >= 0; heapMb >= 0). Negative values are
    // refused (return false, no mutation). The window slides past capacity.
    virtual bool record(double frameMs, double heapMb) = 0;

    // Clears the window and zeroes all stats (heap peak also reset).
    virtual void clear() = 0;

    // Current statistics over the window (deterministic).
    virtual ProfilerSnapshot snapshot() const = 0;

    // Deterministic JSON snapshot ({"samples":N,"min":...,"fps":...}).
    virtual std::string to_json() const = 0;
};

// Creates the profiler with a sliding window of `capacity` frames and a spike
// threshold of `spikeThresholdMs`. capacity == 0 -> default 600 (10s @ 60fps).
// spikeThresholdMs <= 0 -> spikes disabled. Never returns nullptr.
std::unique_ptr<IFrameProfiler> create_frame_profiler(
    std::size_t capacity, double spikeThresholdMs);

}  // namespace profiling
}  // namespace engine
