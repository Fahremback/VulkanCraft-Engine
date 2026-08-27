#!/usr/bin/env node
// spdlog-gate.mjs - section 7 / BUG-018: prove spdlog is a REAL usable logger.
// spdlog is header-only; this gate compiles tools/portability/spdlog-probe.cpp
// against the vendored headers (g++, no lib) and RUNS it, verifying captured
// formatted log lines. Exit 0 = usable as a library (configuration + evidence).
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

// Harden against PATH: discover MinGW and stage its DLLs next to the exe.
const { buildEnv, stageDlls } = await import('./mingw-env.mjs');
const env = buildEnv();

const ROOT = process.cwd();
const SPD = join(ROOT, 'external', 'solutions', 'spdlog');
const PROBE = join(ROOT, 'tools', 'portability', 'spdlog-probe.cpp');
const OUT = process.platform === 'win32' ? join(process.env.TEMP ?? ROOT, 'spdlog-probe.exe') : '/tmp/spdlog-probe';

if (!existsSync(join(SPD, 'include', 'spdlog', 'spdlog.h'))) {
  console.error('[spdlog-gate] FAIL - vendored spdlog headers missing');
  process.exit(1);
}
if (!existsSync(PROBE)) {
  console.error('[spdlog-gate] FAIL - missing tools/portability/spdlog-probe.cpp');
  process.exit(1);
}

const compiler = spawnSync('g++', ['-std=c++17', '-O2', '-pthread', `-I${join(SPD, 'include')}`, PROBE, '-o', OUT], { encoding: 'utf8', env });
if (compiler.status !== 0) {
  console.error(`[spdlog-gate] FAIL - probe did not compile:\n${compiler.stderr || compiler.stdout}`);
  process.exit(1);
}

stageDlls(OUT); // loader searches exe dir for libstdc++/libwinpthread first
const run = spawnSync(OUT, [], { encoding: 'utf8', env });
if (run.status !== 0 || !/spdlog-consumer-ok/.test(run.stdout || '')) {
  console.error(`[spdlog-gate] FAIL - probe run: exit=${run.status} out=${(run.stdout||'').trim()}`);
  process.exit(1);
}

console.log(`[spdlog-gate] PASS - ${run.stdout.trim()}`);
