#!/usr/bin/env node
// immer-gate.mjs — §7 (immer): the vendored immer library (external/solutions/immer)
// is header-only and needs no CMake wiring to be USED. This gate compiles and
// runs tools/portability/immer-probe.cpp against the vendored headers (C++17),
// proving the persistent data structures (vector/flex_vector/box/atom) work —
// the same probe pattern AGENT-1 used for embree (#262). Exit 0 = usable.
//
// Requires a C++17 compiler on PATH (g++/clang++). Pure, deterministic, no IO.
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const IMMER = join(ROOT, 'external', 'solutions', 'immer');
const PROBE = join(ROOT, 'tools', 'portability', 'immer-probe.cpp');
const OUT = process.platform === 'win32' ? join(process.env.TEMP ?? ROOT, 'immer-probe.exe') : '/tmp/immer-probe';

if (!existsSync(join(IMMER, 'immer', 'vector.hpp'))) {
  console.error('[immer-gate] FAIL — vendored immer missing external/solutions/immer/immer/vector.hpp');
  process.exit(1);
}

const compiler = spawnSync('g++', ['-std=c++17', `-I${IMMER}`, PROBE, '-o', OUT], { encoding: 'utf8' });
if (compiler.status !== 0) {
  console.error(`[immer-gate] FAIL — probe did not compile against the vendored headers:\n${compiler.stderr || compiler.stdout}`);
  process.exit(1);
}

const run = spawnSync(OUT, [], { encoding: 'utf8' });
if (run.status !== 0) {
  console.error(`[immer-gate] FAIL — probe exited ${run.status}: ${run.stderr || run.stdout}`);
  process.exit(1);
}

console.log(`[immer-gate] PASS — ${run.stdout.trim()}`);
