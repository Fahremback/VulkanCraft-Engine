#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const root = process.cwd();
const manifest = {
  schema: 1,
  purpose: 'Agent-6 integration evidence contract; implementation phase only',
  platform: 'windows',
  solutions: {
    concurrentqueue: { consumer: 'src/engine/sdk/JobSystem.cpp', test: 'tests/JobSystemTests.cpp', cmake: 'vc_sdk_public' },
    'google-benchmark': { consumer: 'tests/BenchmarkEngineTest.cpp', test: 'benchmark_engine_test', cmake: 'benchmark_engine_test' },
    googletest: { consumer: 'tests/JobSystemTests.cpp', test: 'job_system_tests', cmake: 'gtest_discover_tests' },
    mimalloc: { consumer: 'tools/portability/mimalloc-probe.cpp', test: 'mimalloc-gate', cmake: 'VC_USE_MIMALLOC' },
    spdlog: { consumer: 'src/engine/core/logging/Log.hpp', test: 'spdlog-gate', cmake: 'VC_USE_SPDLOG' },
    taskflow: { consumer: 'tools/portability/taskflow-gate.mjs', test: 'taskflow-gate', cmake: 'external/solutions/taskflow' },
    tracy: { consumer: 'tools/portability/tracy-gate.mjs', test: 'tracy-gate', cmake: 'TRACY_ENABLE' },
    fuzztest: { consumer: 'tools/portability/fuzz-smoke.mjs', test: 'fuzz-smoke', cmake: 'fuzz-smoke' },
    vcpkg: { consumer: 'vcpkg.json', test: 'vcpkg manifest validation', cmake: 'VCPKG_FEATURE_FLAGS' }
  }
};
const output = process.argv[2] ?? 'out/artifacts/integration-manifest.json';
const target = path.resolve(root, output);
fs.mkdirSync(path.dirname(target), { recursive: true });
fs.writeFileSync(target, JSON.stringify(manifest, null, 2) + '\n');
console.log(`integration-manifest: wrote ${Object.keys(manifest.solutions).length} solution contracts to ${output}`);
