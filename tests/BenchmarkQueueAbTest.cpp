// BenchmarkQueueAbTest.cpp — A/B gate (item §9/144) for §7/111: is the
// lock-free concurrentqueue worth activating over the engine's typical
// std::deque event-queue pattern on THIS hardware? Numbers decide; the
// integration is only adopted where a measurable gain appears.
#include <benchmark/benchmark.h>

#include <concurrentqueue.h>

#include <deque>
#include <cstdint>

namespace {
constexpr int kItems = 1 << 12;
}

// --- Baseline: single-threaded std::deque push/pop (engine event-queue style)
static void BM_StdDequePushPop(benchmark::State& state) {
    for (auto _ : state) {
        std::deque<std::uint64_t> q;
        for (int i = 0; i < kItems; ++i) q.push_back(static_cast<std::uint64_t>(i));
        std::uint64_t acc = 0;
        while (!q.empty()) { acc += q.front(); q.pop_front(); }
        benchmark::DoNotOptimize(acc);
    }
}
BENCHMARK(BM_StdDequePushPop);

// --- Candidate: single-threaded concurrentqueue (weak producer/consumer)
static void BM_CqSinglePushPop(benchmark::State& state) {
    for (auto _ : state) {
        moodycamel::ConcurrentQueue<std::uint64_t> q;
        for (int i = 0; i < kItems; ++i) q.enqueue(static_cast<std::uint64_t>(i));
        std::uint64_t acc = 0;
        std::uint64_t v = 0;
        while (q.try_dequeue(v)) { acc += v; }
        benchmark::DoNotOptimize(acc);
    }
}
BENCHMARK(BM_CqSinglePushPop);

// --- Candidate: multi-threaded — each thread enqueues kItems/threads and
// dequeues the same amount (4 threads = 4 producers + 4 consumers contention)
static void BM_CqMultiPushPop(benchmark::State& state) {
    moodycamel::ConcurrentQueue<std::uint64_t> q;
    const int per = kItems / static_cast<int>(state.threads());
    for (auto _ : state) {
        for (int i = 0; i < per; ++i) q.enqueue(static_cast<std::uint64_t>(i));
        std::uint64_t acc = 0;
        std::uint64_t v = 0;
        for (int i = 0; i < per; ++i) { if (q.try_dequeue(v)) acc += v; }
        benchmark::DoNotOptimize(acc);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * per * 2);
}
BENCHMARK(BM_CqMultiPushPop)->Threads(4)->UseRealTime();

BENCHMARK_MAIN();
