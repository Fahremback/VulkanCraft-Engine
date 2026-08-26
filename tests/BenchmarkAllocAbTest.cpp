// BenchmarkAllocAbTest.cpp — A/B gate (item §9/144) for §7/116: does mimalloc
// beat the system allocator on allocation churn on THIS machine? Raw
// malloc/mi_malloc comparisons isolate the allocator cost from C++ wrappers.
#include <benchmark/benchmark.h>

#include <mimalloc.h>

#include <cstdlib>

namespace {
constexpr int kChurn = 1024;
}

// --- Baseline: system malloc/free, uniform 64B (small alloc hot path)
static void BM_SysMallocFree64(benchmark::State& state) {
    for (auto _ : state) {
        void* p[kChurn];
        for (int i = 0; i < kChurn; ++i) p[i] = std::malloc(64);
        for (int i = 0; i < kChurn; ++i) std::free(p[i]);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_SysMallocFree64);

// --- Candidate: mimalloc, uniform 64B
static void BM_MiMallocFree64(benchmark::State& state) {
    for (auto _ : state) {
        void* p[kChurn];
        for (int i = 0; i < kChurn; ++i) p[i] = mi_malloc(64);
        for (int i = 0; i < kChurn; ++i) mi_free(p[i]);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_MiMallocFree64);

// --- Baseline: system malloc/free, mixed sizes (64..4096) — fragmentation
//     sensitive, where allocators differ most.
static void BM_SysMallocFreeMixed(benchmark::State& state) {
    for (auto _ : state) {
        void* p[kChurn];
        for (int i = 0; i < kChurn; ++i) p[i] = std::malloc(64 + (i % 32) * 128);
        for (int i = 0; i < kChurn; ++i) std::free(p[i]);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_SysMallocFreeMixed);

// --- Candidate: mimalloc, mixed sizes
static void BM_MiMallocFreeMixed(benchmark::State& state) {
    for (auto _ : state) {
        void* p[kChurn];
        for (int i = 0; i < kChurn; ++i) p[i] = mi_malloc(64 + (i % 32) * 128);
        for (int i = 0; i < kChurn; ++i) mi_free(p[i]);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_MiMallocFreeMixed);

BENCHMARK_MAIN();
