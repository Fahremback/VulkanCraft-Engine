// BenchmarkSmokeTest.cpp — proves google-benchmark integration works
#include <benchmark/benchmark.h>
#include <vector>
#include <algorithm>
#include <numeric>

static void BM_VectorPushBack(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<int> v;
        for (int i = 0; i < state.range(0); ++i) {
            v.push_back(i);
        }
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_VectorPushBack)->Range(8, 1 << 16);

static void BM_Sort(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<int> v(state.range(0));
        std::iota(v.begin(), v.end(), 0);
        std::reverse(v.begin(), v.end());
        benchmark::DoNotOptimize(v.data());
        std::sort(v.begin(), v.end());
        benchmark::DoNotOptimize(v.data());
    }
}
BENCHMARK(BM_Sort)->Range(8, 1 << 16);

BENCHMARK_MAIN();
