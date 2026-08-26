// BenchmarkLogAbTest.cpp — A/B gate (item §9/144) for §7/117: does spdlog
// (sync or async) justify replacing the engine's raw-fprintf logging? Measured
// with both targets writing to a discard sink (fprintf → NUL, spdlog →
// null_sink) so only the logging path cost is compared.
#define SPDLOG_HEADER_ONLY
#include <spdlog/spdlog.h>
#include <spdlog/async.h>            // async_logger + thread_pool + async_overflow_policy
#include <spdlog/sinks/null_sink.h>

#include <benchmark/benchmark.h>

#include <cstdio>
#include <memory>

namespace {
constexpr int kMessages = 1 << 14;  // 16k messages per iteration
}

// --- Baseline: fprintf to the null device (engine's raw-printf pattern)
static void BM_PrintfNull(benchmark::State& state) {
    FILE* sink = std::fopen("NUL", "w");
    if (!sink) sink = std::fopen("/dev/null", "w");
    for (auto _ : state) {
        for (int i = 0; i < kMessages; ++i) {
            std::fprintf(sink, "msg %d %f %s\n", i, i * 0.5, "event");
        }
    }
    if (sink) std::fclose(sink);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kMessages);
}
BENCHMARK(BM_PrintfNull);

// --- Candidate: spdlog sync (null sink)
static void BM_SpdlogSyncNull(benchmark::State& state) {
    auto logger = spdlog::null_logger_mt("bench-sync");
    logger->set_level(spdlog::level::info);
    for (auto _ : state) {
        for (int i = 0; i < kMessages; ++i) {
            logger->info("msg {} {} {}", i, i * 0.5, "event");
        }
    }
    spdlog::drop_all();
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kMessages);
}
BENCHMARK(BM_SpdlogSyncNull);

// --- Candidate: spdlog async (1 worker, null sink)
static void BM_SpdlogAsyncNull(benchmark::State& state) {
    auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "bench-async", null_sink, spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    spdlog::register_logger(logger);
    logger->set_level(spdlog::level::info);
    for (auto _ : state) {
        for (int i = 0; i < kMessages; ++i) {
            logger->info("msg {} {} {}", i, i * 0.5, "event");
        }
    }
    logger->flush();
    spdlog::drop_all();
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kMessages);
}
BENCHMARK(BM_SpdlogAsyncNull);

BENCHMARK_MAIN();
