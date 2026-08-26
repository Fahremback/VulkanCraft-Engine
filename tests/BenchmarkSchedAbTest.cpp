// BenchmarkSchedAbTest.cpp — A/B gate (item §9/144) for §7/118: does taskflow's
// persistent thread pool beat ad-hoc std::thread spawn per parallel-for on
// THIS machine? The engine's no-scheduler pattern spawns threads per pass.
#include <benchmark/benchmark.h>

#include <taskflow/taskflow.hpp>
#include <taskflow/algorithm/for_each.hpp>  // algorithm.hpp only pulls core/graph.hpp in this fork — without this include for_each_index is declared but never defined (LNK2019)

#include <thread>
#include <vector>
#include <cmath>
#include <atomic>

namespace {
constexpr int kElems = 1 << 20;   // 1M elements
constexpr int kThreads = 4;
}

// --- Baseline: ad-hoc threads per pass (engine's no-scheduler pattern)
static void BM_StdThreadParallelFor(benchmark::State& state) {
    std::vector<double> data(kElems);
    for (int i = 0; i < kElems; ++i) data[i] = static_cast<double>(i);
    std::atomic<double> acc{0.0};
    for (auto _ : state) {
        acc.store(0.0);
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&, t] {
                const int per = kElems / kThreads;
                const int lo = t * per, hi = (t + 1) * per;
                double s = 0.0;
                for (int i = lo; i < hi; ++i) s += std::sqrt(data[i]) * std::sin(data[i]);
                acc.fetch_add(s, std::memory_order_relaxed);
            });
        }
        for (auto& th : ts) th.join();
        benchmark::DoNotOptimize(acc.load());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kElems);
}
BENCHMARK(BM_StdThreadParallelFor);

// --- Candidate: taskflow persistent executor, parallel_for
static void BM_TaskflowParallelFor(benchmark::State& state) {
    std::vector<double> data(kElems);
    for (int i = 0; i < kElems; ++i) data[i] = static_cast<double>(i);
    tf::Executor executor(kThreads);
    std::atomic<double> acc{0.0};
    for (auto _ : state) {
        acc.store(0.0);
        tf::Taskflow flow;
        flow.for_each_index(0, kElems, 1, [&](int i) {
            acc.fetch_add(std::sqrt(data[i]) * std::sin(data[i]), std::memory_order_relaxed);
        });
        executor.run(flow).wait();
        benchmark::DoNotOptimize(acc.load());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kElems);
}
BENCHMARK(BM_TaskflowParallelFor);

BENCHMARK_MAIN();
