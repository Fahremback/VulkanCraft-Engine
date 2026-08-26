// BenchmarkEngineTest.cpp — real engine benchmark against the PUBLIC SDK
// (engine/hashing/IHashProvider, BLAKE3-backed). Proves the google-benchmark
// integration pattern with a genuine engine consumer: configuration, test
// target and reproducible numbers (item §7/114 baseline).
#include <benchmark/benchmark.h>

#include <engine/hashing/IHashProvider.hpp>

#include <string>

static void BM_Blake3Hash64B(benchmark::State& state) {
    auto provider = engine::hashing::create_blake3_hash_provider();
    std::string data(64, 'x');
    for (auto _ : state) {
        benchmark::DoNotOptimize(provider->hash(data));
    }
}
BENCHMARK(BM_Blake3Hash64B);

static void BM_Blake3Hash4KiB(benchmark::State& state) {
    auto provider = engine::hashing::create_blake3_hash_provider();
    std::string data(4096, 'x');
    for (auto _ : state) {
        benchmark::DoNotOptimize(provider->hash(data));
    }
}
BENCHMARK(BM_Blake3Hash4KiB);

static void BM_Blake3HashHex(benchmark::State& state) {
    auto provider = engine::hashing::create_blake3_hash_provider();
    std::string data(256, 'x');
    for (auto _ : state) {
        benchmark::DoNotOptimize(provider->hash_hex(data));
    }
}
BENCHMARK(BM_Blake3HashHex);

static void BM_ToHex32B(benchmark::State& state) {
    std::string bytes(32, static_cast<char>(0xAB));
    for (auto _ : state) {
        benchmark::DoNotOptimize(engine::hashing::to_hex(bytes));
    }
}
BENCHMARK(BM_ToHex32B);

BENCHMARK_MAIN();
