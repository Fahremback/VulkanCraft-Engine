// SystemTimelineTests.cpp — headless gate for the PUBLIC per-system CPU frame
// breakdown contract (ISystemTimeline). Deterministic aggregation: per-system
// sliding windows + min/max/avg/p95/p99, frame-total window, JSON snapshot,
// refusals, determinism across instances. No clocks, no GPU.

#include "engine/profiling/ISystemTimeline.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main() {
    using namespace engine::profiling;

    // --- basic frame: two systems + frame total ---
    {
        auto t = create_system_timeline(0);
        t->begin_frame();
        t->record_system("streaming", 1.2);
        t->record_system("meshing", 2.9);
        t->end_frame(16.7);  // budget 16.7ms; idle = 16.7 - 4.1

        SystemTimelineSnapshot s = t->snapshot();
        check(s.frameCount == 1, "one frame closed");
        check(s.systems.size() == 2, "two systems recorded");
        check(s.systems[0].name == "streaming", "system order = first appearance");
        check(s.systems[0].frames == 1, "streaming frames == 1");
        check(near(s.systems[0].minMs, 1.2, 0.0), "streaming min == 1.2");
        check(near(s.systems[0].maxMs, 1.2, 0.0), "streaming max == 1.2");
        check(near(s.systems[0].avgMs, 1.2, 0.0), "streaming avg == 1.2");
        check(near(s.systems[0].p95Ms, 1.2, 0.0), "streaming p95 == 1.2");
        check(near(s.systems[0].p99Ms, 1.2, 0.0), "streaming p99 == 1.2");
        check(near(s.systems[1].avgMs, 2.9, 0.0), "meshing avg == 2.9");
        check(near(s.totalAvgMs, 16.7, 0.0), "frame total avg == 16.7");
        check(near(s.totalP95Ms, 16.7, 0.0), "frame total p95 == 16.7");
        check(near(s.totalP99Ms, 16.7, 0.0), "frame total p99 == 16.7");
    }

    // --- record_system replaces within a frame (caller records once/system) ---
    {
        auto t = create_system_timeline(0);
        t->begin_frame();
        t->record_system("physics", 0.5);
        t->record_system("physics", 2.5);  // replace, not accumulate
        t->end_frame(5.0);
        SystemTimelineSnapshot s = t->snapshot();
        check(near(s.systems[0].avgMs, 2.5, 0.0), "physics avg == 2.5 (replaced, not summed)");
        check(s.systems[0].frames == 1, "physics one frame");
    }

    // --- avg over 3 frames ---
    {
        auto t = create_system_timeline(0);
        for (int i = 1; i <= 3; ++i) {
            t->begin_frame();
            t->record_system("gen", static_cast<double>(i) * 1.0);
            t->end_frame(10.0);
        }
        SystemTimelineSnapshot s = t->snapshot();
        check(s.systems[0].frames == 3, "gen 3 frames");
        check(near(s.systems[0].minMs, 1.0, 0.0), "gen min == 1.0");
        check(near(s.systems[0].maxMs, 3.0, 0.0), "gen max == 3.0");
        check(near(s.systems[0].avgMs, 2.0, 0.0), "gen avg == 2.0");
        check(near(s.systems[0].p95Ms, 3.0, 0.0), "gen p95 (3 frames) == 3.0");
        check(near(s.systems[0].p99Ms, 3.0, 0.0), "gen p99 (3 frames) == 3.0");
    }

    // --- p95/p99 over a 10-frame window (nearest-rank) ---
    {
        auto t = create_system_timeline(0);
        for (int i = 1; i <= 10; ++i) {
            t->begin_frame();
            t->record_system("r", static_cast<double>(i));
            t->end_frame(100.0);
        }
        SystemTimelineSnapshot s = t->snapshot();
        // sorted 1..10; nearest-rank p95 = rank ceil(9.5)-1 = 9 -> index 9 = 10.
        check(near(s.systems[0].p95Ms, 10.0, 0.0),
              "p95 of 1..10 == 10 (nearest-rank ceil(9.5))");
        check(near(s.systems[0].p99Ms, 10.0, 0.0), "p99 of 1..10 == 10");
        check(near(s.totalAvgMs, 100.0, 0.0), "frame total avg == 100.0");
        check(near(s.totalP95Ms, 100.0, 0.0), "frame total p95 == 100.0");
    }

    // --- sliding window: capacity 3 keeps last 3 ---
    {
        auto t = create_system_timeline(3);
        for (int i = 1; i <= 5; ++i) {
            t->begin_frame();
            t->record_system("w", static_cast<double>(i));
            t->end_frame(100.0);
        }
        SystemTimelineSnapshot s = t->snapshot();
        check(s.systems[0].frames == 3, "window capacity 3 keeps 3 samples");
        check(near(s.systems[0].minMs, 3.0, 0.0), "window min == 3.0");
        check(near(s.systems[0].maxMs, 5.0, 0.0), "window max == 5.0");
        check(s.frameCount == 5, "frameCount still 5 (window is per-system, counter is total)");
    }

    // --- begin_frame drops unclosed partial data ---
    {
        auto t = create_system_timeline(0);
        t->begin_frame();
        t->record_system("zombie", 9.9);  // never closed
        t->begin_frame();                 // aborts the zombie frame
        t->record_system("clean", 1.0);
        t->end_frame(5.0);
        SystemTimelineSnapshot s = t->snapshot();
        // A system recorded but never closed contributes NO samples, so it is
        // omitted from the snapshot (empty windows are hidden — same rule as
        // IRenderPassMetrics). Only the clean system has a closed frame.
        check(s.systems.size() == 1, "unclosed zombie hidden; only clean present");
        check(s.systems[0].name == "clean", "clean system present");
        check(s.systems[0].frames == 1, "clean frames == 1");
        check(near(s.systems[0].avgMs, 1.0, 0.0), "clean avg == 1.0");
    }

    // --- refusals: no mutation ---
    {
        auto t = create_system_timeline(0);
        check(!t->record_system("", 1.0), "empty system refused");
        check(!t->record_system("x", -1.0), "negative ms refused");
        check(!t->record_system("x", std::nan("")), "NaN ms refused");
        check(!t->end_frame(-1.0), "negative frame total refused");
        check(!t->end_frame(std::nan("")), "NaN frame total refused");
        SystemTimelineSnapshot s = t->snapshot();
        check(s.systems.empty(), "refusals did not register systems");
        check(s.frameCount == 0, "refusals did not close frames");
    }

    // --- determinism: two instances identical snapshot + JSON ---
    {
        auto a = create_system_timeline(0);
        auto b = create_system_timeline(0);
        a->begin_frame();
        a->record_system("physics", 0.7);
        a->record_system("submission", 1.5);
        a->end_frame(16.7);
        b->begin_frame();
        b->record_system("physics", 0.7);
        b->record_system("submission", 1.5);
        b->end_frame(16.7);
        check(a->snapshot().systems.size() == b->snapshot().systems.size(),
              "snapshot systems size identical");
        check(a->to_json() == b->to_json(), "JSON bit-identical across instances");
    }

    // --- JSON shape ---
    {
        auto t = create_system_timeline(0);
        t->begin_frame();
        t->record_system("streaming", 1.2);
        t->record_system("idle", 0.9);
        t->end_frame(16.7);
        const std::string json = t->to_json();
        check(json.find("\"frames\":1") != std::string::npos, "json has frame count");
        check(json.find("\"total\"") != std::string::npos, "json has total");
        check(json.find("\"systems\":[") != std::string::npos, "json has systems");
        check(json.find("streaming") != std::string::npos, "json has system name");
        check(json.find("idle") != std::string::npos, "json has idle system");
    }

    // --- clear resets everything ---
    {
        auto t = create_system_timeline(0);
        t->begin_frame();
        t->record_system("gen", 1.0);
        t->end_frame(10.0);
        t->clear();
        SystemTimelineSnapshot s = t->snapshot();
        check(s.systems.empty(), "clear empties systems");
        check(s.frameCount == 0, "clear resets frameCount");
        check(near(s.totalAvgMs, 0.0, 0.0), "clear resets total avg");
    }

    if (g_failures == 0) {
        std::printf("[system-timeline] ALL PASSED\n");
        return 0;
    }
    std::printf("[system-timeline] %d FAILURE(S)\n", g_failures);
    return 1;
}