// Gate for engine::profiling::IFrameProfiler (agente 2 §B) — deterministic
// frame-time/memory stats. Headless: no window, no clock, no RNG.

#include "engine/profiling/IFrameProfiler.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

#define CHECK_NEAR(a, b, eps, msg)                                      \
    do {                                                                \
        const double da = (a), db = (b);                                \
        if ((da < db - (eps)) || (da > db + (eps))) {                   \
            std::printf("FAIL %s:%d — %s (%.4f vs %.4f)\n", __FILE__,  \
                        __LINE__, msg, da, db);                         \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

void test_empty() {
    auto p = engine::profiling::create_frame_profiler(4, 50.0);
    const auto snap = p->snapshot();
    CHECK(snap.samples == 0, "empty window");
    CHECK(snap.minMs == 0.0 && snap.maxMs == 0.0 && snap.avgMs == 0.0,
          "empty stats zeroed");
    CHECK(snap.p95Ms == 0.0 && snap.p99Ms == 0.0, "empty percentiles zeroed");
    CHECK(snap.fps == 0.0, "empty fps zeroed");
    CHECK(snap.spikeCount == 0, "no spikes");
}

void test_basic_stats() {
    auto p = engine::profiling::create_frame_profiler(100, 50.0);
    // 5 frames: 10, 20, 30, 40, 50 ms @ 256, 512 MB.
    CHECK(p->record(10.0, 256.0), "record 1");
    CHECK(p->record(20.0, 512.0), "record 2");
    CHECK(p->record(30.0, 640.0), "record 3");
    CHECK(p->record(40.0, 128.0), "record 4");
    CHECK(p->record(50.0, 256.0), "record 5");

    const auto snap = p->snapshot();
    CHECK(snap.samples == 5, "5 samples");
    CHECK_NEAR(snap.minMs, 10.0, 1e-9, "min");
    CHECK_NEAR(snap.maxMs, 50.0, 1e-9, "max");
    CHECK_NEAR(snap.avgMs, 30.0, 1e-9, "avg");
    CHECK_NEAR(snap.fps, 1000.0 / 50.0, 1e-9, "fps from last frame");
    CHECK_NEAR(snap.heapMb, 256.0, 1e-9, "heap from last frame");
    CHECK_NEAR(snap.heapPeakMb, 640.0, 1e-9, "heap peak");
    // Sorted: 10,20,30,40,50. p95 nearest-rank: rank=4 -> 50. p99 -> 50.
    CHECK_NEAR(snap.p95Ms, 50.0, 1e-9, "p95 = max for small window");
    CHECK_NEAR(snap.p99Ms, 50.0, 1e-9, "p99 = max for small window");
}

void test_percentiles() {
    auto p = engine::profiling::create_frame_profiler(100, 999.0);
    // 10 frames: 1..10 ms (sorted). p95 nearest-rank: rank=9 -> 10.0.
    for (int i = 1; i <= 10; ++i) {
        CHECK(p->record(static_cast<double>(i), 0.0), "record frame");
    }
    const auto snap = p->snapshot();
    CHECK_NEAR(snap.p95Ms, 10.0, 1e-9, "p95 nearest-rank (10 of 10)");
    CHECK_NEAR(snap.p99Ms, 10.0, 1e-9, "p99 nearest-rank (10 of 10)");
    CHECK_NEAR(snap.minMs, 1.0, 1e-9, "min");
    CHECK_NEAR(snap.maxMs, 10.0, 1e-9, "max");

    // 20 frames: 1..20 -> p95 rank=19 -> 20.0.
    auto q = engine::profiling::create_frame_profiler(100, 999.0);
    for (int i = 1; i <= 20; ++i) {
        q->record(static_cast<double>(i), 0.0);
    }
    CHECK_NEAR(q->snapshot().p95Ms, 20.0, 1e-9, "p95 of 20 frames");
}

void test_window_slide() {
    auto p = engine::profiling::create_frame_profiler(3, 999.0);
    p->record(10.0, 100.0);
    p->record(20.0, 200.0);
    p->record(30.0, 300.0);
    CHECK(p->snapshot().samples == 3, "window full at 3");

    p->record(40.0, 400.0);  // slides: drops 10
    const auto snap = p->snapshot();
    CHECK(snap.samples == 3, "window stays at 3");
    CHECK_NEAR(snap.minMs, 20.0, 1e-9, "oldest dropped");
    CHECK_NEAR(snap.maxMs, 40.0, 1e-9, "newest kept");
}

void test_spikes() {
    auto p = engine::profiling::create_frame_profiler(100, 50.0);
    p->record(10.0, 0.0);
    p->record(60.0, 0.0);  // spike
    p->record(20.0, 0.0);
    p->record(100.0, 0.0);  // spike
    p->record(49.9, 0.0);
    p->record(50.1, 0.0);  // spike (strictly above)
    CHECK(p->snapshot().spikeCount == 3, "3 spikes counted");
}

void test_refusals_and_clear() {
    auto p = engine::profiling::create_frame_profiler(10, 50.0);
    CHECK(!p->record(-1.0, 0.0), "negative frameMs refused");
    CHECK(!p->record(10.0, -5.0), "negative heap refused");
    CHECK(p->snapshot().samples == 0, "refusals do not mutate");

    p->record(10.0, 100.0);
    p->record(80.0, 300.0);  // spike
    CHECK(p->snapshot().spikeCount == 1, "spike before clear");
    p->clear();
    const auto snap = p->snapshot();
    CHECK(snap.samples == 0 && snap.spikeCount == 0, "clear zeroes");
    CHECK(snap.heapPeakMb == 0.0, "clear resets heap peak");
}

void test_determinism() {
    auto a = engine::profiling::create_frame_profiler(50, 33.0);
    auto b = engine::profiling::create_frame_profiler(50, 33.0);
    const double frames[] = { 16.7, 16.9, 17.1, 40.0, 16.5, 33.1, 16.8 };
    for (int i = 0; i < 7; ++i) {
        a->record(frames[i], 100.0 + i);
        b->record(frames[i], 100.0 + i);
    }
    CHECK(a->snapshot() == b->snapshot(), "snapshot equality cross-instance");
    CHECK(a->to_json() == b->to_json(), "JSON determinism cross-instance");
}

void test_json() {
    auto p = engine::profiling::create_frame_profiler(10, 50.0);
    p->record(10.0, 256.0);
    p->record(20.0, 512.0);
    const std::string json = p->to_json();
    CHECK(json.find("\"samples\":2") != std::string::npos, "json samples");
    CHECK(json.find("\"fps\":50.000") != std::string::npos, "json fps");
    CHECK(json.find("\"heap_peak_mb\":512.000") != std::string::npos,
          "json heap peak");
}

}  // namespace

int main() {
    test_empty();
    test_basic_stats();
    test_percentiles();
    test_window_slide();
    test_spikes();
    test_refusals_and_clear();
    test_determinism();
    test_json();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
