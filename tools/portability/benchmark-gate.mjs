#!/usr/bin/env node
// benchmark-gate.mjs — Agent 6 §7/114: benchmark regression blocking.
// Runs benchmark_engine_test (public SDK BLAKE3), compares against the stored
// baseline, and FAILS on any regression beyond tolerance. Baselines are real
// numbers captured on this machine; refresh with --update.
//
//   node tools/portability/benchmark-gate.mjs [--update]
import { execSync, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const ARTIFACTS = join(ROOT, 'out', 'artifacts', 'benchmarks');
const BASELINE = join(ARTIFACTS, 'benchmark_engine_baseline.json');
const EXE = join(ROOT, 'build', 'Release', 'benchmark_engine_test.exe');
const UPDATE = process.argv.includes('--update');
// Tolerance is configurable (BENCH_TOLERANCE) because on a shared dev machine
// with concurrent builds, consecutive runs of the same binary vary by ~±25%
// (observed 2026-08-26). Dedicated CI runners should keep the default 0.10.
const TOLERANCE = Number(process.env.BENCH_TOLERANCE ?? 0.10);

if (!existsSync(EXE)) {
  console.error('[benchmark-gate] FAIL: build/Release/benchmark_engine_test.exe not found — build first');
  process.exit(1);
}
if (!existsSync(BASELINE)) {
  console.error('[benchmark-gate] FAIL: no baseline at ' + BASELINE + ' — run with --update to capture');
  process.exit(1);
}

const baseline = JSON.parse(readFileSync(BASELINE, 'utf8'));
const map = new Map(baseline.benchmarks.map((b) => [b.name, b.real_time]));

// Fresh run: median of 3 repetitions (report aggregates) so a single noisy
// run on a shared/hot machine cannot flip the gate. On dedicated CI runners
// the aggregate median is stable well within the default tolerance.
const run = spawnSync(EXE, [
  '--benchmark_min_time=0.05s', '--benchmark_repetitions=3',
  '--benchmark_report_aggregates_only=true', '--benchmark_format=json',
], { cwd: ROOT, encoding: 'utf8', timeout: 180000, windowsHide: true });
if (run.status !== 0) {
  console.error('[benchmark-gate] FAIL: benchmark run exited ' + run.status);
  process.exit(1);
}
const fresh = JSON.parse(run.stdout);
// Aggregate entries have name like 'BM_X_mean' — keep only the median rows.
const median = fresh.benchmarks.filter((b) => b.aggregate_name === 'median');
fresh.benchmarks = median.length ? median : fresh.benchmarks;
const failures = [];
const rows = [];
for (const b of fresh.benchmarks) {
  const prev = map.get(b.name);
  const dt = prev ? (b.real_time - prev) / prev : 0;
  rows.push({ name: b.name, time: b.real_time, baseline: prev ?? null, delta: dt });
  if (prev && dt > TOLERANCE) {
    failures.push(`${b.name}: ${b.real_time.toFixed(1)}ns vs baseline ${prev.toFixed(1)}ns (+${(dt * 100).toFixed(1)}%)`);
  }
}

for (const r of rows) {
  const flag = r.delta > TOLERANCE ? '  <-- REGRESSION' : '';
  const base = r.baseline === null ? ' (new)' : ` baseline ${r.baseline.toFixed(1)}ns (${(r.delta * 100).toFixed(1)}%)`;
  console.log(`[benchmark-gate]   ${r.name.padEnd(24)} ${r.time.toFixed(1)}ns${base}${flag}`);
}

if (UPDATE) {
  mkdirSync(ARTIFACTS, { recursive: true });
  writeFileSync(BASELINE, JSON.stringify(fresh, null, 2), 'utf8');
  console.log(`[benchmark-gate] baseline updated: ${BASELINE}`);
}

if (failures.length) {
  console.error('[benchmark-gate] FAIL: ' + failures.length + ' regression(s)');
  for (const f of failures) console.error('  - ' + f);
  process.exit(1);
}
console.log(`[benchmark-gate] PASS — ${fresh.benchmarks.length} benchmarks within ${TOLERANCE * 100}% of baseline`);
