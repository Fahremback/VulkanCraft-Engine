#!/usr/bin/env node
// sqlite-gate.mjs — §7 gate de utilização do SQLite vendido (finding #297).
// Compila e roda tools/portability/sqlite-probe.c contra o amalgamation
// gerado (external/solutions/sqlite/sqlite3.c + sqlite3.h). Exit 0 =
// utilizável; 1 = não compila/roda. Mesmo padrão do immer-gate (#288).
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const SQLITE = join(ROOT, 'external', 'solutions', 'sqlite');
const PROBE = join(ROOT, 'tools', 'portability', 'sqlite-probe.c');
const AMALG = join(SQLITE, 'sqlite3.c');
const EXE = join(process.env.TEMP || '/tmp', 'sqlite-probe-gate');

if (!existsSync(AMALG)) {
  // Auto-genera o amalgamation pela cadeia canônica reprodutível.
  const gen = spawnSync(process.execPath, [join(ROOT, 'tools', 'portability', 'sqlite-amalgamate.mjs')],
    { encoding: 'utf8' });
  if (gen.status !== 0 || !existsSync(AMALG)) {
    console.error('[sqlite-gate] FAIL: amalgamation missing and generation failed');
    console.error(gen.stdout + gen.stderr);
    process.exit(1);
  }
}

const cc = process.env.CC || 'gcc';
const build = spawnSync(cc, ['-O1', '-w', '-I', SQLITE, PROBE, AMALG, '-o', EXE],
  { encoding: 'utf8' });
if (build.status !== 0) {
  console.error('[sqlite-gate] FAIL: compile');
  console.error(build.stderr || build.stdout);
  process.exit(1);
}

const run = spawnSync(EXE, [], { encoding: 'utf8' });
if (run.status !== 0) {
  console.error('[sqlite-gate] FAIL: run');
  console.error(run.stdout + run.stderr);
  process.exit(1);
}
process.stdout.write(run.stdout);
console.log('[sqlite-gate] PASS — vendored sqlite usable (open/exec/prepare/bind/step)');
process.exit(0);
