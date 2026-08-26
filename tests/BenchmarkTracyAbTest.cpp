// BenchmarkTracyAbTest.cpp — A/B gate (item §9/144) for §7/119: what is the
// real per-zone cost of tracy when the client is enabled but no profiler is
// connected? If zone overhead is small relative to real work, adopting tracy
// zones is safe; if it dominates, zones should be debug-only.
#define TRACY_ENABLE
#include <tracy/Tracy.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>

namespace {
constexpr int kIters = 1 << 10;  // 1024 zone creates per iteration
}

static std::uint64_t work(std::uint64_t x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

// --- Baseline: plain loop (no profiling)
static void BM_PlainLoop(benchmark::State& state) {
    std::uint64_t acc = 1;
    for (auto _ : state) {
        for (int i = 0; i < kIters; ++i) acc = work(acc + i);
        benchmark::DoNotOptimize(acc);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kIters);
}
BENCHMARK(BM_PlainLoop);

// --- Candidate: same loop, each iteration inside a tracy scoped zone
static void BM_TracyZonedLoop(benchmark::State& state) {
    std::uint64_t acc = 1;
    for (auto _ : state) {
        for (int i = 0; i < kIters; ++i) {
            ZoneScoped;
            acc = work(acc + i);
        }
        benchmark::DoNotOptimize(acc);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kIters);
}
BENCHMARK(BM_TracyZonedLoop);

BENCHMARK_MAIN();
