#!/usr/bin/env node
// freshness-gate.mjs — AGENT-6 §9: fails when a binary/library/SDK/MCP/package
// is OLDER than the sources that compose it. Prevents stale-artifact
// conclusions ("the exe exists" when the code moved on). Compares mtimes of
// key outputs vs the newest source they depend on.
//
//   node tools/portability/freshness-gate.mjs [--root-dir DIR]
//
// Exit 0 = everything fresh (or source strictly older). Exit 1 = stale output.
import { readdirSync, statSync, existsSync } from 'node:fs';
import { join, relative } from 'node:path';

const ROOT = process.cwd();
const buildDir = process.env.VC_BUILD_DIR || 'build';
const problems = [];

// Outputs that must be at least as new as the newest source under their dep
// roots. Scans are bounded: public headers + core src for the SDK manifests,
// src/engine/sdk + src/simulation/voxel for the world-save header, and the
// whole src/ for the built exes in build/Release and out/release.
const OUTPUTS = [
  { label: 'SDK_API_INVENTORY.md (anti-drift manifest)', path: 'docs/SDK_API_INVENTORY.md', deps: ['src/engine/public', 'schema/registry'] },
  { label: 'SDK_CAPABILITY_MATRIX.md (capability manifest)', path: 'docs/SDK_CAPABILITY_MATRIX.md', deps: ['src/engine/public', 'tools/mcp-server'] },
  { label: 'BINDINGS_GENERATED.md (bindings doc)', path: 'docs/BINDINGS_GENERATED.md', deps: ['src/engine/public', 'tools/mcp-server', 'schema/registry'] },
  { label: 'world_save_generated.h (schema codegen)', path: 'out/release/generated/world_save_generated.h', deps: ['src/engine/sdk/world_save.fbs'] },
];

function newestSource(deps) {
  let newest = 0;
  for (const d of deps) {
    const abs = join(ROOT, d);
    if (!existsSync(abs)) { problems.push(`dep root missing: ${d}`); continue; }
    const st = statSync(abs);
    if (st.isFile()) { newest = Math.max(newest, st.mtimeMs); continue; }
    const stack = [abs];
    while (stack.length) {
      const dir = stack.pop();
      let entries;
      try { entries = readdirSync(dir, { withFileTypes: true }); } catch { continue; }
      for (const e of entries) {
        const p = join(dir, e.name);
        if (e.isDirectory()) stack.push(p);
        else {
          if (!/\.(cpp|hpp|h|fbs|json|mjs|ts)$/.test(e.name)) continue;
          const s = statSync(p);
          if (s.mtimeMs > newest) newest = s.mtimeMs;
        }
      }
    }
  }
  return newest;
}

for (const out of OUTPUTS) {
  const outAbs = join(ROOT, out.path);
  if (!existsSync(outAbs)) { problems.push(`${out.label}: MISSING (${out.path})`); continue; }
  const outTime = statSync(outAbs).mtimeMs;
  const srcTime = newestSource(out.deps);
  if (srcTime > outTime + 1000) {  // 1s tolerance for clock granularity
    const delta = ((srcTime - outTime) / 1000).toFixed(0);
    problems.push(`${out.label}: STALE by ${delta}s — source newer than generated output (regenerate via its tool)`);
  }
}

// Built exes vs newest source in the tree (bounded scan of src/).
const srcRoot = join(ROOT, 'src');
let newestSrc = newestSource(['src']);
for (const exe of [`${buildDir}/Release/VulkanEngineGame.exe`, `${buildDir}/Release/VulkanEngineServer.exe`,
                    'out/release/VulkanEngineGame.exe', 'out/release/Release/VulkanEngineGame.exe']) {
  if (!existsSync(join(ROOT, exe))) continue;
  const t = statSync(join(ROOT, exe)).mtimeMs;
  if (newestSrc > t + 1000) {
    problems.push(`${exe}: STALE — src/ newer than the binary (rebuild before declaring the target done)`);
  }
}

if (problems.length) {
  console.error('[freshness-gate] FAIL:');
  problems.forEach((p) => console.error(`  - ${p}`));
  process.exit(1);
}
console.log(`[freshness-gate] PASS — all generated outputs and binaries fresh relative to their sources (${OUTPUTS.length} outputs checked).`);
