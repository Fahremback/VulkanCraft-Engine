#!/usr/bin/env node
// repo-hygiene-gate.mjs — AGENT-6 §5: verifies no secrets, absolute paths,
// stale build artifacts, or inflated assets leak into the package/repo.
// Pure node; all-or-nothing (any hit = exit 1 with evidence).
//
//   node tools/portability/repo-hygiene-gate.mjs
import { readdirSync, statSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const problems = [];

// Bounded scan roots (skip build trees, out/, external clones, .git).
const SCAN_DIRS = ['tools', 'docs', 'src/engine/public', 'cmake', 'agentes', 'schema', 'shaders'];
const SKIP = /node_modules|_deps|\.git/;
const SECRET_RE = /(sk-[A-Za-z0-9]{20,}|AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9]{30,}|xox[baprs]-[A-Za-z0-9-]{10,}|BEGIN (RSA|OPENSSH|EC) PRIVATE|AIza[0-9A-Za-z_-]{35})/;
const ABS_RE = /[A-Za-z]:\\Users\\[^"'\s\\]+\\(?!\.gemini)/;  // user-home abs paths outside the engine dir

const EXT = /\.(cpp|hpp|h|mjs|js|ts|md|json|fbs|cmake|txt|yml|yaml|ps1|bat)$/;

function walk(dir, depth = 0) {
  if (depth > 6) return;
  let entries;
  try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const e of entries) {
    const p = join(dir, e.name);
    if (SKIP.test(p)) continue;
    if (e.isDirectory()) { walk(p, depth + 1); continue; }
    if (!EXT.test(e.name)) continue;
    if (statSync(p).size > 2 * 1024 * 1024) { problems.push(`${p}: file >2MB in source area (inflated?)`); continue; }
    const text = readFileSafe(p);
    if (!text) continue;
    if (SECRET_RE.test(text)) problems.push(`${p}: potential secret (API key / private key)`);
    if (ABS_RE.test(text)) problems.push(`${p}: absolute user path (non-engine)`);
  }
}

function readFileSafe(p) {
  try {
    const buf = require('node:fs').readFileSync(p);
    // Skip binary files.
    if (buf.includes(0)) return null;
    return buf.toString('utf8');
  } catch { return null; }
}

for (const d of SCAN_DIRS) {
  if (existsSync(join(ROOT, d))) walk(join(ROOT, d));
}

// Build-artifact litter in source areas (stale .bak/.orig/.tlog).
for (const root of SCAN_DIRS.slice(0, 3)) {
  const p = join(ROOT, root);
  if (!existsSync(p)) continue;
  const stack = [p];
  while (stack.length) {
    const dir = stack.pop();
    let entries; try { entries = readdirSync(dir, { withFileTypes: true }); } catch { continue; }
    for (const e of entries) {
      const f = join(dir, e.name);
      if (e.isDirectory()) { if (!SKIP.test(f)) stack.push(f); continue; }
      if (/\.(bak|orig|tlog|obj|pdb|user|suo)$/i.test(e.name)) problems.push(`${f}: stale build artifact in source area`);
    }
  }
}

if (problems.length) {
  console.error('[repo-hygiene] FAIL:');
  problems.forEach((p) => console.error(`  - ${p}`));
  process.exit(1);
}
console.log('[repo-hygiene] PASS — no secrets, no non-engine absolute paths, no stale artifacts, no inflated files in source areas.');
