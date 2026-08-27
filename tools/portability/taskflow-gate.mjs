#!/usr/bin/env node
// taskflow-gate.mjs - section 7 / BUG-018: prove Taskflow is a REAL usable task-graph library.
// Header-only; this gate compiles tools/portability/taskflow-probe.cpp against the vendored header
// (g++ -std=c++20 -pthread, no lib) and RUNS it, verifying a 3-task DAG (A->B->C) executes with
// correct sequential ordering (counter 111). Exit 0 = usable as a library (real DAG + pool + evidence).
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join, dirname } from 'path';

const ROOT = process.cwd();
const TF = join(ROOT, 'external', 'solutions', 'taskflow');
const PROBE = join(ROOT, 'tools', 'portability', 'taskflow-probe.cpp');
const OUT = process.platform === 'win32' ? join(process.env.TEMP ?? ROOT, 'taskflow-probe.exe') : '/tmp/taskflow-probe';

if (!existsSync(join(TF, 'taskflow', 'taskflow.hpp'))) {
  console.error('[taskflow-gate] FAIL - vendored taskflow/taskflow.hpp missing');
  process.exit(1);
}
if (!existsSync(PROBE)) {
  console.error('[taskflow-gate] FAIL - missing tools/portability/taskflow-probe.cpp');
  process.exit(1);
}

// MinGW UCRT exe needs libstdc++/libwinpthread DLLs at load time. On Windows the loader
// searches the exe's own dir first, so we copy the needed DLLs next to the probe exe —
// deterministic and independent of the bootstrap PATH.
const env = { ...process.env };
try {
  if (process.platform === 'win32') {
    const probeGcc = spawnSync('g++', ['-x', 'c++', '-E', '-v', '-o', '/dev/null', '-'], { input: '', encoding: 'utf8' });
    const text = (probeGcc.stderr || '') + (probeGcc.stdout || '');
    const m = text.match(/COLLECT_GCC=(.*)\n/);
    let binDir = null;
    if (m && m[1]) binDir = dirname(m[1]);
    if (!binDir) {
      // fallback: locate g++ via where.exe
      const where = spawnSync('where', ['g++.exe'], { encoding: 'utf8' });
      if (where.status === 0 && where.stdout) binDir = dirname(where.stdout.trim().split(/\r?\n/)[0]);
    }
    if (binDir) {
      const { copyFileSync } = await import('fs');
      const outDir = dirname(OUT);
      for (const dll of ['libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll']) {
        try { copyFileSync(join(binDir, dll), join(outDir, dll)); } catch (e) { /* optional dll */ }
      }
    }
  }
} catch (e) { /* dll staging is best-effort */ }

const compiler = spawnSync('g++', ['-std=c++20', '-O2', '-pthread', `-I${TF}`, PROBE, '-o', OUT], { encoding: 'utf8', env });
if (compiler.status !== 0) {
  console.error(`[taskflow-gate] FAIL - probe did not compile:\n${compiler.stderr || compiler.stdout}`);
  process.exit(1);
}

const run = spawnSync(OUT, [], { encoding: 'utf8', env });
if (run.status !== 0 || !/taskflow-consumer-ok/.test(run.stdout || '')) {
  console.error(`[taskflow-gate] FAIL - probe run: exit=${run.status} out=${(run.stdout || '').trim()}`);
  process.exit(1);
}

console.log(`[taskflow-gate] PASS - ${run.stdout.trim()}`);