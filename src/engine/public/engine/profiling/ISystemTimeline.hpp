#pragma once

// ISystemTimeline — the PUBLIC headless contract for per-system CPU frame
// breakdown (the "CPU: World streaming / Generation / Meshing / Physics /
// Render submission / Main idle" table the performance phase needs BEFORE any
// optimization). Written by the replay/engine loop once per frame: each named
// system reports the ms it actually consumed this frame; the aggregator keeps
// a sliding window per system and computes min/max/avg/p95/p99 (nearest-rank
// on the sorted window, exactly like IRenderPassMetrics). Pure CPU, fully
// headless-testable, deterministic (no clocks, no RNG, no globals — time only
// enters via recordSystem/frameTotal).
//
// Frame model:
//   - begin_frame() opens a frame.
//   - recordSystem(name, ms) accumulates ONE system's ms for the open frame.
//     Calling it twice for the same system in a frame REPLACES the previous
//     value for that system this frame (the caller is expected to record the
//     accumulated elapsed of each system once, after it runs). Negative/non-
//     finite values and empty names are refused (return false, no mutation).
//   - end_frame(frameTotalMs) closes the frame: frameTotalMs is the whole
//     measured frame budget. Per-system >= 0 (refused otherwise; a system
//     consuming more than the total is allowed and reported as-is). The frame
//     total is stored so the "Main idle/wait = frameTotal - sum(systems)"
//     row can be derived.
//   - clear() empties every window/system.
//
// Determinism: same sequence of begin_frame/recordSystem/end_frame calls
// produces an identical snapshot() and to_json(). Windows slide past capacity
// (oldest dropped first). An explicit "idle" system may be recorded by the
// caller if it wants it persisted as a named system; otherwise it can be
// derived from the sum.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace profiling {

inline constexpr std::size_t kSystemTimelineDefaultWindow = 600;  // 10s @ 60fps

// Per-frame totals for one system over the sliding window.
struct SystemTimelineStats {
    std::string name;
    std::size_t frames{ 0 };      // frames this system was recorded
    double minMs{ 0.0 };
    double maxMs{ 0.0 };
    double avgMs{ 0.0 };
    double p95Ms{ 0.0 };
    double p99Ms{ 0.0 };
};

// One recorded frame's raw values (per system + frame total).
struct SystemFrameSample {
    std::string name;
    double ms{ 0.0 };
};

// Immutable snapshot of the whole timeline.
struct SystemTimelineSnapshot {
    std::vector<SystemTimelineStats> systems;  // insertion order (first appearance)
    std::size_t frameCount{ 0 };               // frames closed since clear
    double totalAvgMs{ 0.0 };                  // average of frameTotalMs over closed frames
    double totalP95Ms{ 0.0 };
    double totalP99Ms{ 0.0 };
};

class ISystemTimeline {
public:
    virtual ~ISystemTimeline() = default;

    // Opens a new frame. Any in-progress partial frame data for systems that
    // were recorded but never closed is discarded. Returns true.
    virtual bool begin_frame() = 0;

    // Records one system's ms for the open frame. Replaces any prior value for
    // the same system this frame. Non-finite/negative/empty refused -> false.
    virtual bool record_system(const std::string& name, double ms) = 0;

    // Closes the frame with the measured frame total. frameTotalMs must be
    // finite and >= 0. Pushes each recorded system's value into its window and
    // the frame total into the global window. Returns false on invalid input
    // (no mutation). Calling without an open frame is a no-op returning false.
    virtual bool end_frame(double frameTotalMs) = 0;

    // Clears all systems, windows and counters.
    virtual void clear() = 0;

    // Deterministic snapshot.
    virtual SystemTimelineSnapshot snapshot() const = 0;

    // Deterministic JSON snapshot (%.6g; key order fixed).
    virtual std::string to_json() const = 0;
};

// Creates the timeline with a sliding window of `capacity` frames per system.
// capacity == 0 -> default (kSystemTimelineDefaultWindow). Never null.
std::unique_ptr<ISystemTimeline> create_system_timeline(std::size_t capacity);

}  // namespace profiling
}  // namespace engine