#!/usr/bin/env node
// sentry-gate.mjs — §7 gate de utilização do sentry-native vendido
// (finding #306). Builda a lib estática MSVC (inproc backend,
// SENTRY_BUILD_SHARED_LIBS=OFF) e compila+roda tools/portability/sentry-probe.c
// (init, transport custom capturando o envelope SEM rede, event com
// tags/release, shutdown com flush event+session, segundo ciclo). Exit 0 =
// crash reporting vendido utilizável SEM wiring — o backend real do ISink de
// observability (#294). Mesmo padrão dos demais gates §7.
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const SENTRY = join(ROOT, 'external', 'solutions', 'sentry-native');
const PROBE_BAT = join(SENTRY, 'build-sentry-probe.bat');
const LIB_BAT = join(SENTRY, 'build-sentry-gate.bat');

const STATIC_LIB = join(SENTRY, 'build-gate', 'Release', 'sentry.lib');
if (!existsSync(STATIC_LIB)) {
  const b = spawnSync(LIB_BAT, { encoding: 'utf8', shell: true, cwd: SENTRY });
  if (b.status !== 0) {
    console.error('[sentry-gate] FAIL: lib build error');
    console.error(b.stdout + b.stderr);
    process.exit(1);
  }
}

const run = spawnSync(PROBE_BAT, { encoding: 'utf8', shell: true, cwd: ROOT });
process.stdout.write(run.stdout);
if (run.status !== 0) {
  console.error('[sentry-gate] FAIL');
  process.stderr.write(run.stderr);
  process.exit(1);
}
console.log('[sentry-gate] PASS — vendored sentry-native usable (init + capture + transport + shutdown)');
process.exit(0);
