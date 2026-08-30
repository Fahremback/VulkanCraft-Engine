#!/usr/bin/env node
/**
 * deps-acquire.mjs — A5 §D-57: reproducible acquisition of vendored
 * dependencies on a fresh Windows machine.
 *
 * The engine ships `external/solutions/*` as a git work tree. This tool
 * verifies that every dependency recorded in manifests/SBOM.json is either
 *   - pinned with an origin (own git work tree) and on disk at that pin, or
 *   - marked `vendored: true` (source already in-tree — no re-acquisition).
 *
 * Optional --clone <name> re-acquires a pinned dependency into
 * external/solutions/<name> from its recorded origin at the recorded pin
 * (`git clone <origin> && git checkout <pin>`).
 *
 * Usage:
 *   node tools/portability/deps-acquire.mjs            # verify all
 *   node tools/portability/deps-acquire.mjs --clone abseil  # re-clone one
 * Exit 0 = all dependencies acquirable/verified; 1 = any missing pin.
 */
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const ROOT = process.cwd();
const SOLUTIONS = join(ROOT, 'external', 'solutions');
const SBOM_PATH = join(ROOT, 'manifests', 'SBOM.json');

function fail(msg) { console.error(`[deps-acquire] FAIL: ${msg}`); process.exit(1); }
function log(m) { console.log(`[deps-acquire] ${m}`); }

if (!existsSync(SBOM_PATH)) fail(`missing ${SBOM_PATH} — run sbom-gen.mjs first`);

const sbom = JSON.parse(readFileSync(SBOM_PATH, 'utf8'));
const deps = sbom.dependencies.filter((d) => d.kind === 'external/solutions');

const cloneName = process.argv.indexOf('--clone') !== -1
  ? process.argv[process.argv.indexOf('--clone') + 1]
  : null;

if (cloneName) {
  const dep = deps.find((d) => d.name === cloneName);
  if (!dep) fail(`'${cloneName}' not in SBOM`);
  if (dep.vendored) fail(`'${cloneName}' is a vendored snapshot (source in-tree) — nothing to clone`);
  if (!dep.origin || !dep.pin) fail(`'${cloneName}' has no origin/pin recorded`);
  const dest = join(SOLUTIONS, cloneName);
  log(`cloning ${dep.origin} @ ${dep.pin.slice(0, 12)} → external/solutions/${cloneName}`);
  const clone = spawnSync('git', ['clone', dep.origin, dest], { encoding: 'utf8', stdio: 'inherit', windowsHide: true });
  if (clone.status !== 0) fail(`git clone failed for ${cloneName}`);
  const co = spawnSync('git', ['-C', dest, 'checkout', dep.pin], { encoding: 'utf8', stdio: 'inherit', windowsHide: true });
  if (co.status !== 0) fail(`git checkout ${dep.pin} failed for ${cloneName}`);
  log(`${cloneName} re-acquired at pin ${dep.pin.slice(0, 12)}`);
  process.exit(0);
}

// Verify mode.
const problems = [];
let pinned = 0;
let vendored = 0;
for (const d of deps) {
  const dir = join(SOLUTIONS, d.name);
  if (d.vendored) {
    if (!existsSync(dir)) problems.push(`${d.name}: marked vendored but missing on disk`);
    else vendored++;
    continue;
  }
  if (!d.origin || !d.pin) { problems.push(`${d.name}: no origin/pin recorded`); continue; }
  if (!existsSync(dir)) { problems.push(`${d.name}: missing on disk (clone with --clone ${d.name})`); continue; }
  const head = spawnSync('git', ['-C', dir, 'rev-parse', 'HEAD'], { encoding: 'utf8', timeout: 5000, windowsHide: true });
  const at = head.status === 0 ? head.stdout.trim() : '';
  if (at !== d.pin) problems.push(`${d.name}: on-disk HEAD ${at.slice(0, 12) || '?'} != pin ${d.pin.slice(0, 12)}`);
  else pinned++;
}

if (problems.length) {
  for (const p of problems) console.error(`  - ${p}`);
  fail(`${problems.length} dependency acquisition problem(s)`);
}
log(`PASS — ${pinned} pinned deps at recorded commit + ${vendored} vendored snapshots in-tree (${deps.length} total).`);
log(`Re-acquisition: node tools/portability/deps-acquire.mjs --clone <name>`);
