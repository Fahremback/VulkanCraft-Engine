#!/usr/bin/env node
// tuf-gate.mjs — §7 gate de utilização do python-tuf vendido (finding #301).
// Roda tools/portability/tuf-probe.py contra a árvore vendida em
// external/solutions/python-tuf (tuf 7.0.0) com a dependência declarada
// (securesystemslib[crypto]) instalada in-tree em .venv-deps. Exit 0 =
// utilizável para atualizações seguras. Mesmo padrão do immer/sqlite/
// libsodium/curl gates.
import { spawnSync } from 'child_process';
import { existsSync, mkdirSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const TUF = join(ROOT, 'external', 'solutions', 'python-tuf');
const PROBE = join(ROOT, 'tools', 'portability', 'tuf-probe.py');
const DEPS = join(TUF, '.venv-deps');

if (!existsSync(DEPS)) {
  console.error('[tuf-gate] FAIL: in-tree deps missing — install with:');
  console.error('  python -m pip install --target external/solutions/python-tuf/.venv-deps "securesystemslib[crypto]"');
  process.exit(1);
}

const run = spawnSync('python', [PROBE], { encoding: 'utf8', cwd: ROOT });
process.stdout.write(run.stdout);
if (run.status !== 0) {
  console.error('[tuf-gate] FAIL');
  process.stderr.write(run.stderr);
  process.exit(1);
}
console.log('[tuf-gate] PASS — vendored python-tuf usable (authoring + client update + tamper/rollback rejection)');
process.exit(0);
