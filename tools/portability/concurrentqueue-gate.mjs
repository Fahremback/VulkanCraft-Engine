#!/usr/bin/env node
// concurrentqueue-gate.mjs - section 7 / BUG-018: prove moodycamel::ConcurrentQueue is a REAL usable queue.
// Header-only; this gate compiles tools/portability/concurrentqueue-probe.cpp against the vendored
// header (g++, -pthread, no lib) and RUNS it, verifying a 2-producer/1-consumer transfer of 200k
// elements with integer-sum integrity. Exit 0 = usable as a library (real concurrency + evidence).
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

// Harden against PATH: discover MinGW and stage its DLLs next to the exe.
const { buildEnv, stageDlls } = await import('./mingw-env.mjs');
const env = buildEnv();

const ROOT = process.cwd();
const CQ = join(ROOT, 'external', 'solutions', 'concurrentqueue');
const PROBE = join(ROOT, 'tools', 'portability', 'concurrentqueue-probe.cpp');
const OUT = process.platform === 'win32' ? join(process.env.TEMP ?? ROOT, 'concurrentqueue-probe.exe') : '/tmp/concurrentqueue-probe';

if (!existsSync(join(CQ, 'concurrentqueue.h'))) {
  console.error('[concurrentqueue-gate] FAIL - vendored concurrentqueue.h missing');
  process.exit(1);
}
if (!existsSync(PROBE)) {
  console.error('[concurrentqueue-gate] FAIL - missing tools/portability/concurrentqueue-probe.cpp');
  process.exit(1);
}

const compiler = spawnSync('g++', ['-std=c++17', '-O2', '-pthread', `-I${CQ}`, PROBE, '-o', OUT], { encoding: 'utf8', env });
if (compiler.status !== 0) {
  console.error(`[concurrentqueue-gate] FAIL - probe did not compile:\n${compiler.stderr || compiler.stdout}`);
  process.exit(1);
}

stageDlls(OUT); // loader searches exe dir for libstdc++/libwinpthread first
const run = spawnSync(OUT, [], { encoding: 'utf8', env });
if (run.status !== 0 || !/concurrentqueue-consumer-ok/.test(run.stdout || '')) {
  console.error(`[concurrentqueue-gate] FAIL - probe run: exit=${run.status} out=${(run.stdout || '').trim()}`);
  process.exit(1);
}

console.log(`[concurrentqueue-gate] PASS - ${run.stdout.trim()}`);