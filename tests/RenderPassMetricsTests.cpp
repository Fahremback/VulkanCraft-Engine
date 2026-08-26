// RenderPassMetricsTests.cpp — Agente 1 (task_plan B): headless gate for the
// PUBLIC renderer-telemetry producer (IRenderPassMetrics). Deterministic
// aggregation: per-pass CPU/GPU windows + min/max/avg/p95/p99, memory pool
// residency, streaming counters, JSON snapshot, refusals, determinism. No GPU.

#include "engine/rendering/IRenderPassMetrics.hpp"

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
    using namespace Engine::Rendering;

    auto m1 = create_render_pass_metrics(0);  // default window
    auto m2 = create_render_pass_metrics(0);

    // --- pass timing: min/max/avg ---
    m1->recordPass("gi", 1.0, 0.5);
    m1->recordPass("gi", 2.0, 1.0);
    m1->recordPass("gi", 3.0, 1.5);
    m1->recordPass("shadows", 0.5, 0.25);
    {
        RenderMetricsSnapshot s = m1->snapshot();
        check(s.passes.size() == 2, "two passes expected");
        const RenderPassStats& gi = s.passes[0];
        check(gi.name == "gi", "pass order = first appearance");
        check(gi.samples == 3, "gi samples == 3");
        check(near(gi.cpuMsMin, 1.0, 0.0), "gi cpu min == 1.0");
        check(near(gi.cpuMsMax, 3.0, 0.0), "gi cpu max == 3.0");
        check(near(gi.cpuMsAvg, 2.0, 0.0), "gi cpu avg == 2.0");
        check(near(gi.cpuMsP95, 3.0, 0.0), "gi cpu p95 (3 samples) == 3.0");
        check(near(gi.cpuMsP99, 3.0, 0.0), "gi cpu p99 (3 samples) == 3.0");
        check(near(gi.gpuMsAvg, 1.0, 0.0), "gi gpu avg == 1.0");
    }

    // --- p95/p99 correctness with a 10-sample window ---
    {
        auto p = create_render_pass_metrics(0);
        for (int i = 1; i <= 10; ++i) {
            p->recordPass("r", static_cast<double>(i), 0.0);
        }
        RenderMetricsSnapshot s = p->snapshot();
        // sorted 1..10; nearest-rank p95 = rank ceil(9.5)-1 = 9 -> 10th (10)
        check(near(s.passes[0].cpuMsP95, 10.0, 0.0),
              "p95 of 1..10 == 10 (nearest-rank ceil(9.5))");
        // p99 = rank ceil(9.9)-1 = 9 -> 10
        check(near(s.passes[0].cpuMsP99, 10.0, 0.0), "p99 of 1..10 == 10");
    }

    // --- sliding window: capacity 3 keeps last 3 ---
    {
        auto p = create_render_pass_metrics(3);
        for (int i = 1; i <= 5; ++i) {
            p->recordPass("w", static_cast<double>(i), 0.0);
        }
        RenderMetricsSnapshot s = p->snapshot();
        check(s.passes[0].samples == 3, "window capacity 3 keeps 3 samples");
        check(near(s.passes[0].cpuMsMin, 3.0, 0.0), "window min == 3.0");
        check(near(s.passes[0].cpuMsMax, 5.0, 0.0), "window max == 5.0");
    }

    // --- memory pools ---
    m1->recordMemory("surface_cache", 100);
    m1->recordMemory("surface_cache", 300);
    m1->recordMemory("gi_probes", 50);
    {
        RenderMetricsSnapshot s = m1->snapshot();
        check(s.pools.size() == 2, "two pools expected");
        check(s.pools[0].name == "surface_cache", "pool order = first appearance");
        check(s.pools[0].samples == 2, "surface_cache samples == 2");
        check(s.pools[0].currentBytes == 300, "surface_cache current == 300");
        check(s.pools[0].minBytes == 100, "surface_cache min == 100");
        check(s.pools[0].maxBytes == 300, "surface_cache max == 300");
        check(s.pools[0].peakBytes == 300, "surface_cache peak == 300");
    }

    // --- streaming: accumulate over frames, peak in-flight ---
    {
        auto p = create_render_pass_metrics(0);
        p->recordStreaming("chunks", 5, 2, 1000);
        p->recordStreaming("chunks", 3, 1, 500);
        p->endFrame();  // frame 1: 8 loaded, 3 evicted, in-flight 5
        p->recordStreaming("chunks", 2, 4, 200);
        p->endFrame();  // frame 2: +2/-4, in-flight 3
        RenderMetricsSnapshot s = p->snapshot();
        check(s.streams.size() == 1, "one stream expected");
        check(s.streams[0].totalLoaded == 10, "totalLoaded == 10");
        check(s.streams[0].totalEvicted == 7, "totalEvicted == 7");
        check(s.streams[0].bytesLoaded == 1700, "bytesLoaded == 1700");
        check(s.streams[0].peakBytesInFlight == 5, "peakInFlight == 5");
    }

    // --- refusals: no mutation ---
    {
        check(!m1->recordPass("", 1.0, 1.0), "empty pass refused");
        check(!m1->recordPass("gi", -1.0, 1.0), "negative cpu refused");
        check(!m1->recordPass("gi", 1.0, std::nan("")), "NaN gpu refused");
        check(!m1->recordMemory("", 1), "empty pool refused");
        check(!m1->recordStreaming("", 1, 0, 0), "empty stream refused");
        RenderMetricsSnapshot s = m1->snapshot();
        check(s.passes.size() == 2, "refusals did not add passes");
    }

    // --- determinism: two instances identical snapshot + JSON ---
    {
        auto a = create_render_pass_metrics(0);
        auto b = create_render_pass_metrics(0);
        a->recordPass("main", 2.5, 1.25);
        a->recordPass("main", 3.5, 1.75);
        a->recordMemory("vram", 1024);
        a->recordStreaming("cards", 4, 1, 800);
        a->endFrame();
        b->recordPass("main", 2.5, 1.25);
        b->recordPass("main", 3.5, 1.75);
        b->recordMemory("vram", 1024);
        b->recordStreaming("cards", 4, 1, 800);
        b->endFrame();
        check(a->to_json() == b->to_json(), "JSON bit-identical across instances");
    }

    // --- JSON shape ---
    {
        auto p = create_render_pass_metrics(0);
        p->recordPass("gi", 1.0, 0.5);
        p->recordMemory("surface_cache", 42);
        p->recordStreaming("chunks", 1, 0, 10);
        p->endFrame();
        const std::string json = p->to_json();
        check(json.find("\"passes\":[") != std::string::npos, "json has passes");
        check(json.find("\"pools\":[") != std::string::npos, "json has pools");
        check(json.find("\"streams\":[") != std::string::npos, "json has streams");
        check(json.find("gi") != std::string::npos, "json has pass name");
        check(json.find("surface_cache") != std::string::npos, "json has pool name");
        check(json.find("chunks") != std::string::npos, "json has stream name");
    }

    // --- clear resets ---
    {
        auto p = create_render_pass_metrics(0);
        p->recordPass("gi", 1.0, 1.0);
        p->recordMemory("vram", 10);
        p->recordStreaming("chunks", 1, 0, 1);
        p->endFrame();
        p->clear();
        RenderMetricsSnapshot s = p->snapshot();
        check(s.passes.empty(), "clear empties passes");
        check(s.pools.empty(), "clear empties pools");
        check(s.streams.empty(), "clear empties streams");
    }

    if (g_failures == 0) {
        std::printf("[render-pass-metrics] ALL PASSED\n");
        return 0;
    }
    std::printf("[render-pass-metrics] %d FAILURE(S)\n", g_failures);
    return 1;
}