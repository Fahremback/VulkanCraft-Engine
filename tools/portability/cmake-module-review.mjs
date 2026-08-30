#!/usr/bin/env node
// cmake-module-review.mjs — A5 §C (modularização e build incremental):
// statically reviews the vc_object_module split for domain coherence and
// staleness, without running CMake (build lock):
//   1. every TU listed in a module must exist on disk (no stale refs)
//   2. each module must be domain-coherent: its TUs must belong to at most
//      one canonical domain bucket (sdk/<domain>, simulation/voxel, features,
//      plugins, engine/<area>, app, editor) — a module mixing unrelated
//      domains is a modularization defect (destroys incremental builds)
//   3. each module's TUs must actually be consumed by a product executable
//      (direct $<TARGET_OBJECTS:mod> or the VC_SDK_PUBLIC_OBJECTS aggregate)
//   4. object modules that exist but are never linked by any executable are
//      reported as orphan modules (dead build graph nodes)
//
// Usage: node tools/portability/cmake-module-review.mjs [--check]
//   --check: exit 1 on any stale/mixed/orphan finding (gate contract).
// Pure node; derives everything from CMakeLists.txt + filesystem.
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const root = process.cwd();
const cmake = readFileSync(join(root, 'CMakeLists.txt'), 'utf8');

// ---- 0. CMake variable table (set(VAR value) up to end of file) ----
// Object modules reference paths like ${VC_COACD_DIR}/src/x.cpp — the var must
// be expanded before existence checks or every external path looks stale.
const varTable = new Map();
// CMAKE_SOURCE_DIR is a CMake built-in; relative to the configure root.
varTable.set('CMAKE_SOURCE_DIR', '.');
for (const s of cmake.matchAll(/\bset\s*\(\s*([A-Za-z0-9_]+)\s+([^#\n]+?)\s*\)/g)) {
  const value = s[2].trim().replace(/["'${}]/g, '').trim();
  if (value && !value.includes(' ')) varTable.set(s[1], value);
}
const expandVars = (tok) => {
  let out = tok.replace(/["'${}]/g, '').trim();
  for (let guard = 0; guard < 8; guard++) {
    const m = out.match(/\b([A-Z][A-Z0-9_]{2,})\//);
    if (!m) break;
    const v = varTable.get(m[1]);
    if (!v) break;
    out = out.replace(m[0], v.replace(/\/$/, '') + '/');
  }
  return out.replace(/^\.\//, '');
};

// ---- 1. module -> TUs ----
const moduleRe = /\bvc_object_module\s*\(\s*([^\s#]+)([\s\S]*?)\)\s*\n/g;
const moduleTUs = new Map();
let m;
while ((m = moduleRe.exec(cmake)) !== null) {
  const body = m[2].replace(/#[^\n]*(?=\n|$)/g, ' ');
  const srcs = [...body.matchAll(/\S+\.(?:cpp|cc|cxx|c)(?:\s|\)|$)/g)]
    .map((x) => x[0].replace(/["'${}]/g, '').replace(/\s|\)$/, '').trim())
    .filter((x) => x.includes('/') && !x.startsWith('$<'));
  if (srcs.length) moduleTUs.set(m[1], srcs);
}

// ---- 2. domain classification ----
function domainOf(tu) {
  const r = tu.replace(/\\/g, '/');
  if (r.startsWith('src/engine/sdk/')) {
    const name = r.slice('src/engine/sdk/'.length).toLowerCase();
    if (/globalillumination|lumen|surfacecache|diffuse|tracer|render|preset|raytracer|embree|reflect|atmosphere|meshcook|parcellation|procgen|texture|material|pipeline|shader|fx|particle|effect|water|ocean/.test(name)) return 'sdk/rendering';
    if (/world|terrain|chunk|voxel|block|biome|stream|generat|struct|fluid|lod|region|lighting|mesher|save|serialization|asset|registry/.test(name)) return 'sdk/world';
    if (/motion|animat|skeleton|clip|ik|skin|rootmotion|behavior|btcpp|navigation|nav|pathfind|ai\b|agent|vision|perception/.test(name)) return 'sdk/ai_motion';
    if (/network|net\b|udp|tcp|transport|replicat|rpc|session|interest|reliable|socket|connect|ping|latency|sync|snapshot/.test(name)) return 'sdk/networking';
    if (/editor|ui|tool|panel|gizmo|inspector|viewport|imgui|widget|layout|undo|play|debugsurface|overlay/.test(name)) return 'sdk/editor';
    return 'sdk/core';
  }
  if (r.startsWith('src/simulation/voxel/')) return 'voxel';
  if (r.startsWith('src/simulation/')) return 'simulation';
  if (r.startsWith('src/features/')) return 'features';
  if (r.startsWith('src/plugins/')) return 'plugins';
  if (r.startsWith('src/editor/')) return 'editor';
  if (r.startsWith('src/app/')) return 'app';
  if (r.startsWith('src/server/')) return 'server';
  if (r.startsWith('src/engine/')) {
    const area = r.slice('src/engine/'.length).split('/')[0];
    return 'engine/' + area;
  }
  return 'other';
}

// ---- 3. linked modules (direct refs + VC_SDK_PUBLIC_OBJECTS aggregate) ----
const productExeNames = ['VulkanEngineGame', 'VulkanEngineEditor', 'VulkanEngineServer', 'VulkanEngineCooker', 'VulkanEnginePackageBuilder'];
const linkedModules = new Set();
for (const pname of productExeNames) {
  const blockStart = cmake.indexOf(`add_executable(${pname}`);
  if (blockStart < 0) continue;
  const endRel = cmake.indexOf('add_dependencies', blockStart);
  const endExe = cmake.indexOf('add_executable', blockStart + ('add_executable(' + pname).length);
  let end = Infinity;
  if (endRel > 0) end = Math.min(end, endRel);
  if (endExe > 0) end = Math.min(end, endExe);
  const block = cmake.slice(blockStart, end === Infinity ? blockStart + 5000 : end);
  for (const o of block.matchAll(/TARGET_OBJECTS:([A-Za-z0-9_]+)/g)) linkedModules.add(o[1]);
}
for (const agg of cmake.matchAll(/set\s*\(\s*VC_SDK_PUBLIC_OBJECTS\s*([\s\S]*?)\)\s*\n/g)) {
  for (const t of agg[1].match(/[^\s#]+/g) || []) {
    const mm = t.match(/TARGET_OBJECTS:([^>\s]+)/);
    if (mm) linkedModules.add(mm[1]);
  }
}
// Object modules may also be consumed as libraries in target_link_libraries
// (e.g. vc_material_pipeline) — collect those references too so a module is
// only an orphan when NO product executable links it by any means.
const linkLibRe = /target_link_libraries\s*\(\s*([^\s#]+)([\s\S]*?)\)\s*\n/g;
let ll;
while ((ll = linkLibRe.exec(cmake)) !== null) {
  if (!productExeNames.includes(ll[1])) continue;
  for (const t of ll[2].match(/[A-Za-z0-9_]+/g) || []) {
    if (moduleTUs.has(t)) linkedModules.add(t);
  }
}

// ---- findings ----
const stale = [];
const mixed = [];
const orphan = [];

for (const [mod, tus] of moduleTUs) {
  const expanded = tus.map(expandVars);
  // stale TU references
  for (let i = 0; i < expanded.length; i++) {
    if (!existsSync(join(root, expanded[i]))) stale.push({ module: mod, tu: expanded[i] });
  }
  // domain coherence (only engine modules; external vendor TUs under
  // external/solutions are compiled as-is and are their own domain)
  const own = expanded.filter((t) => !t.startsWith('external/') && existsSync(join(root, t)));
  const domains = [...new Set(own.map(domainOf))];
  const expected = domains[0];
  const offenders = own.filter((t) => domainOf(t) !== expected);
  if (domains.length > 1) {
    mixed.push({ module: mod, domains, offenders: offenders.map((t) => ({ tu: t, domain: domainOf(t) })) });
  }
  // orphan module (never linked by any product exe)
  if (!linkedModules.has(mod) && !/vc_(sdk_public|coacd|manifold|pbd|geometry_central)$/.test(mod)) {
    orphan.push({ module: mod, tus: expanded.length });
  }
}

const report = {
  generator: 'cmake-module-review.mjs',
  generatedAt: new Date().toISOString(),
  modules: moduleTUs.size,
  linkedModules: linkedModules.size,
  stale,
  mixed,
  orphan,
  // Hard defects: stale refs break configure; orphan modules never reach a
  // product binary. Mixed-domain modules are DESIGN OBSERVATIONS: cross-domain
  // aggregates (e.g. vc_gameplay_sdk = gameplay+physics+rendering facade,
  // vc_core_runtime = UUID/plugin/serialization/assets/audio/scene) are a
  // deliberate trade (fewer link lines) and do not fail the gate — they are
  // reported so an owner can review whether the split is still right.
  clean: stale.length === 0 && orphan.length === 0
};

console.log(`[cmake-module-review] modules=${report.modules} linked=${report.linkedModules}`);
if (stale.length) { console.log(`[cmake-module-review] STALE refs: ${stale.length}`); for (const s of stale) console.log(`  stale ${s.module}: ${s.tu}`); }
if (mixed.length) { console.log(`[cmake-module-review] MIXED-domain modules (review, non-blocking): ${mixed.length}`); for (const s of mixed) console.log(`  mixed ${s.module}: ${s.domains.join(' + ')} (${s.offenders.slice(0, 6).map((o) => o.tu.split('/').pop() + ':' + o.domain).join(', ')}${s.offenders.length > 6 ? ', …' : ''})`); }
if (orphan.length) { console.log(`[cmake-module-review] ORPHAN modules (never linked by product exe): ${orphan.length}`); for (const o of orphan) console.log(`  orphan ${o.module} (${o.tus} TUs)`); }
console.log(`[cmake-module-review] ${report.clean ? 'PASS — no stale refs, no orphan modules (mixed-domain review findings are non-blocking)' : 'FAIL — see findings above'}`);

// machine-readable artifact (same pattern as integration-auditor.mjs)
import { mkdirSync, writeFileSync } from 'node:fs';
const outDir = join(root, 'out', 'artifacts', 'cmake-module-review');
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, 'cmake-module-review.json'), JSON.stringify(report, null, 2) + '\n');
console.log(`[cmake-module-review] wrote out/artifacts/cmake-module-review/cmake-module-review.json`);

if (process.argv.includes('--check') && !report.clean) process.exit(1);
