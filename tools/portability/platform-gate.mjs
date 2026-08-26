#!/usr/bin/env node
// platform-gate.mjs — §11 aggregate gate (AGENT-6): runs ALL SDK/MCP platform
// gates in one invocation with a single exit code, so CI/team can prove the
// whole §11 surface green together. Each step is a separate tool with its own
// evidence; this orchestrates them in dependency-safe order.
//
//   node tools/portability/platform-gate.mjs [--skip-build] [--skip-spaces]
//
// --skip-build skips the C++ semantic_api_tests exe (parity) and the
// demo/spaces installs that require build/ — for use on trees without build/.
import { spawnSync } from 'child_process';
import { join } from 'path';
import { existsSync } from 'fs';

const ROOT = process.cwd();
const args = process.argv.slice(2);
const SKIP_BUILD = args.includes('--skip-build');
const SKIP_SPACES = args.includes('--skip-spaces');

const STEPS = [
  ['ci-lint (ci.yml valid + gates exist)', 'node', ['tools/portability/ci-lint.mjs']],
  ['error-registry', 'node', ['tools/portability/error-registry.mjs']],
  ['bindings-gen', 'node', ['tools/portability/bindings-gen.mjs']],
  ['asset-pipeline', 'node', ['tools/portability/asset-pipeline.mjs', '--project', 'PlatformGate_Pipeline']],
  ['script-gate (valid+invalid fixtures)', 'node', ['tools/portability/script-gate-fixture.mjs']],
  ['gen-docs', 'node', ['tools/portability/gen-docs.mjs']],
  ['parity (C+++CLI+MCP)', 'node', ['tools/portability/parity-tests.mjs', 'game_capabilities', 'list_game_projects']],
  ['fuzz-smoke', 'node', ['tools/portability/fuzz-smoke.mjs']],
  ['concurrency', 'node', ['tools/portability/concurrency-tests.mjs']],
  ['demo prompt→game', 'node', ['tools/portability/demo-prompt-game.mjs']],
  ['spaces-path gate', 'node', ['tools/portability/spaces-path-gate.mjs']],
];

function log(m) { console.log(`[platform-gate] ${m}`); }
function fail(m) { console.error(`[platform-gate] FAIL: ${m}`); process.exitCode = 1; }

function run(name, cmd, argsList) {
  log(`▶ ${name}`);
  const res = spawnSync(cmd, argsList, {
    cwd: ROOT, encoding: 'utf8', timeout: 600000, windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const tail = ((res.stdout || '') + (res.stderr || '')).trim().split('\n').slice(-3).join('\n');
  if (res.status === 0) {
    log(`  ✓ ${name} PASSED`);
    return true;
  }
  console.error(tail);
  fail(`${name} (exit ${res.status})`);
  return false;
}

console.log(`# §11 platform gate (AGENT-6) — ${new Date().toISOString()}`);
console.log(`skip-build=${SKIP_BUILD} skip-spaces=${SKIP_SPACES}\n`);

if (!existsSync(join(ROOT, 'build'))) {
  fail('build/ not found — configure first (or use --skip-build for non-parity steps)');
}

let failures = 0;
for (const [name, cmd, argsList] of STEPS) {
  if (name === 'parity (C+++CLI+MCP)' && SKIP_BUILD) { log('  (skipped: --skip-build)'); continue; }
  if (name === 'spaces-path gate' && SKIP_SPACES) { log('  (skipped: --skip-spaces)'); continue; }
  if (name === 'demo prompt→game' && SKIP_BUILD) { log('  (skipped: --skip-build)'); continue; }
  if (!run(name, cmd, argsList)) failures++;
}

if (failures) {
  console.error(`\nPLATFORM GATE FAILED (${failures} step(s))`);
  process.exit(1);
}
console.log('\nPLATFORM GATE PASSED — all §11 SDK/MCP gates green together.');
