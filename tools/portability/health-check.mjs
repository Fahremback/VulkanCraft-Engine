#!/usr/bin/env node
// health-check.mjs — AGENT-6 §11 one-shot swarm health check: verifies the
// whole integration surface in one command so ANY agent can prove the tree is
// green before handing off or declaring DONE. Lighter than ci-matrix.mjs
// (which reconfigures+rebuilds): this runs the fast verification gates only.
//
//   node tools/portability/health-check.mjs [--skip-build]
//
// Steps (fast first, so a broken tree fails early):
//   1. ci-lint            — ci.yml valid + gates exist (BUG-015 class)
//   2. sdk-check          — public headers self-contained (AGENT-5 anti-drift)
//   3. protocol-smoke     — MCP wire + CLI + auth (AGENT-5)
//   4. fast-gate unit     — 31 unit tests (~1.3s)
//   5. external-consumer  — 5 SDK consumers build+run (needs build/)
//   6. platform-gate      — 11 §11 gates (parity needs build/)
//
// --skip-build skips the two steps that require built exes (external-consumer,
// platform-gate parity/demo) for use on trees without build/.
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { existsSync } from 'node:fs';

const ROOT = process.cwd();
const SKIP_BUILD = process.argv.includes('--skip-build');
// Canonical shared tree (out/dev-shared); the in-source build/ is legacy.
const BUILD_ROOT = process.env.VC_BUILD_DIR || 'out/dev-shared';
const HAS_BUILD = existsSync(join(ROOT, BUILD_ROOT)) || existsSync(join(ROOT, 'build'));

const STEPS = [
  ['ci-lint', 'node', ['tools/portability/ci-lint.mjs'], false],
  ['sdk-check', 'node', ['tools/sdk/sdk-check.mjs'], false],
  ['protocol-smoke', 'node', ['tools/mcp-server/protocol-smoke.mjs'], false],
  ['fast-gate unit', 'node', ['tools/portability/fast-gate.mjs', 'unit'], false],
  ['external-consumer', 'node', ['tools/portability/external-consumer-gate.mjs'], true],
  ['platform-gate', 'node', SKIP_BUILD
    ? ['tools/portability/platform-gate.mjs', '--skip-build']
    : ['tools/portability/platform-gate.mjs'], true],
];

function run(name, cmd, argsList) {
  console.log(`\n[health] ▶ ${name}`);
  // Propagate VC_BUILD_DIR to sub-gates so they resolve the shared out-of-tree
  // build instead of the legacy in-tree build/ (BUG-038 drift).
  const env = { ...process.env };
  const res = spawnSync(cmd, argsList, {
    cwd: ROOT, encoding: 'utf8', timeout: 900000, windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'], env,
  });
  const tail = ((res.stdout || '') + (res.stderr || '')).trim().split('\n').slice(-3).join('\n');
  if (res.status === 0) { console.log(`[health]   ✓ ${name} PASSED`); return true; }
  console.error(tail);
  console.error(`[health]   ✗ ${name} FAILED (exit ${res.status})`);
  return false;
}

console.log(`# Swarm health check — ${new Date().toISOString()}`);
console.log(`build/ present=${HAS_BUILD} skip-build=${SKIP_BUILD}`);

let failures = 0;
for (const [name, cmd, argsList, needsBuild] of STEPS) {
  if (needsBuild && !HAS_BUILD) { console.log(`[health]   (skipped: no build/ — ${name})`); continue; }
  if (needsBuild && SKIP_BUILD && name !== 'platform-gate') { console.log(`[health]   (skipped: --skip-build — ${name})`); continue; }
  if (!run(name, cmd, argsList)) failures++;
}

if (failures) {
  console.error(`\n[health] HEALTH CHECK FAILED (${failures} step(s))`);
  process.exit(1);
}
console.log('\n[health] HEALTH CHECK PASSED — integration surface green.');
