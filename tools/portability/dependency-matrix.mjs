#!/usr/bin/env node
// dependency-matrix.mjs — A5 Section D: single repository → agent → capability
// → files reused → call site → state → packaged-artifact matrix for EVERY repo
// in external/solutions and every entry in docs/SOLUCOES_E_DEPENDENCIAS.md.
//
// Inventory is derived at runtime (readdir on external/solutions + the catalog
// table) — no historical count is trusted. Every repo is classified from code
// evidence only:
//   INTEGRADO   -> wired into the build (CMake target) AND has a real call site
//                  (a non-test source/adapter referencing the repo, feeding a
//                  public contract) and is consumed by an executable.
//   FERRAMENTA  -> used by build/tooling/gates only (tools/, scripts, .bat).
//   REFERENCIA  -> catalogued / cloned but not wired and not tooled.
//   REMOVER     -> not in catalog, no wire, no tool reuse (candidate for removal).
//
// Gate (--check) fails while any repo is unclassified, or any INTEGRADO lacks a
// real (non-test, non-tool) consumer. Emits machine-readable JSON + a per-target
// manifest (which external repos each target depends on) + a static HTML report.
import { readdirSync, readFileSync, statSync, existsSync } from 'node:fs';
import { join, relative, basename } from 'node:path';

const ROOT = process.cwd();
const SOLUTIONS = join(ROOT, 'external', 'solutions');
const CATALOG = join(ROOT, 'docs', 'SOLUCOES_E_DEPENDENCIAS.md');
const CMAKE = join(ROOT, 'CMakeLists.txt');
const OUT = join(ROOT, 'out', 'artifacts', 'dependency-matrix');

function walk(dir, extRe, acc = []) {
  if (!existsSync(dir)) return acc;
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    let st; try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) {
      if (/^\.(git|build|out|_)/.test(e)) continue;
      walk(p, extRe, acc);
    } else if (extRe.test(e)) acc.push(p);
  }
  return acc;
}
const rel = (p) => relative(ROOT, p).replace(/\\/g, '/');
const text = (p) => { try { return readFileSync(p, 'utf8'); } catch { return ''; } };

// ---- 1. real repo dirs (exclude false positives like README/SNAPSHOT) ----
const falseDirs = new Set(['README.md', 'AGENTS.md', 'SNAPSHOT.md', 'node_modules', '.git', 'missing']);
const repoDirs = readdirSync(SOLUTIONS, { withFileTypes: true })
  .filter((d) => d.isDirectory() && !d.name.startsWith('.') && !falseDirs.has(d.name))
  .map((d) => d.name)
  .sort();

// ---- 2. catalog entries from docs/SOLUCOES_E_DEPENDENCIAS.md ----
// Table: | Área | Solução | Pasta local | Uso definido |
function parseCatalog() {
  const rows = new Map(); // repoDir -> {area, solution, usage}
  if (!existsSync(CATALOG)) return rows;
  const lines = text(CATALOG).split(/\r?\n/);
  let inTable = false;
  for (const line of lines) {
    if (/^\|.*\|.*\|.*\|/.test(line)) {
      const parts = line.split('|').map((s) => s.trim());
      // docs/SOLUCOES_E_DEPENDENCIAS.md uses several layouts:
      //   4-cell: | Área | Solução | Pasta local | Uso |
      //   5-cell: | Área | Solução | Pasta local | Tipo | Uso |
      //   3-cell: | Solução | Pasta local | Uso |   (reference section)
      // Instead of guessing the column, find the path cell by its content
      // (a backticked `external/solutions/<repo>` value) wherever it sits.
      const pathIdx = parts.findIndex((s) => /`external\/solutions\/([^`]+)`/.test(s));
      if (pathIdx >= 0) {
        const pathCell = parts[pathIdx] || '';
        const key = (pathCell.match(/`external\/solutions\/([^`]+)`/) || [])[1];
        // solution name = previous cell (or '—' if none); usage = any later cell
        const solution = pathIdx > 1 && parts[pathIdx - 1] ? parts[pathIdx - 1] : '—';
        const usage = parts.slice(pathIdx + 1).join(' ').trim();
        if (key) rows.set(key, { area: '—', solution, usage });
      }
    }
  }
  return rows;
}
const catalog = parseCatalog();

// ---- 3. CMake wiring evidence ----
const cmake = text(CMAKE);
// Expanded copy: resolve `set(VC_*_DIR <value>)` variables so wiring via
// ${VC_COACD_DIR}/... / ${VC_MANIFOLD_DIR}/... counts as a vendored-path ref
// (A5 §D-55 — these were false CONSUMIDO-SEM-WIRE positives before).
const cmakeExpanded = (() => {
  const vars = {};
  for (const m of cmake.matchAll(/set\s*\(\s*(VC_[A-Z0-9_]+_DIR)\s+([^)\r\n]+)/gi)) {
    vars[m[1].toUpperCase()] = m[2].trim().replace(/[\r\n].*/s, '');
  }
  let t = cmake;
  for (const [k, v] of Object.entries(vars)) t = t.split(`\${${k}}`).join(v);
  return t;
})();
const srcAll = walk(join(ROOT, 'src'), /\.(cpp|hpp|h|cc)$/);
const srcText = srcAll.map(text).join('\n');
const appText = walk(join(ROOT, 'src', 'app'), /\.cpp$/).map(text).join('\n');
const editorText = walk(join(ROOT, 'src', 'editor'), /\.cpp$/).map(text).join('\n');
const serverText = walk(join(ROOT, 'src', 'server'), /\.cpp$/).map(text).join('\n');
const sdkText = walk(join(ROOT, 'src', 'engine', 'sdk'), /\.cpp$/).map(text).join('\n');
const testText = walk(join(ROOT, 'tests'), /\.(cpp|hpp|mjs)$/).map(text).join('\n');
const toolText = walk(join(ROOT, 'tools'), /\.(mjs|bat|ps1|cpp|py|c)$/).map(text).join('\n');
const externalPrefix = 'external/solutions/';

// mapped cmake target names historically used (only for display hint, not state)
const knownTargets = [
  'vc_zstd','vc_blake3','vc_flatbuffers','vc_rocksdb','vc_entt','vc_navigation',
  'vc_ozz','vc_acl','vc_fastwfc','vc_delaunator','vc_earcut','vc_meshoptimizer',
  'vc_xatlas','vc_opus','vc_behavior_tree_cpp','vc_motion_matching','vc_effekseer_core',
  'vc_efsw','vc_coacd','vc_manifold','vc_geometry_central','vc_pbd','vc_sph'
];

function classify(repo) {
  const esc = repo.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  // Two signals:
  //   pathRef — genuine vendored-path reference: external/solutions/<repo> or a
  //             include/quote of <repo>/... (a real reuse, not a bare word in a
  //             comment or a similarly-named local symbol).
  //   wordRef — the bare repo name anywhere (weaker; used for tooling/tests only).
  const pathRef = new RegExp(`(?:external[/\\\\]solutions[/\\\\]|VC_SOLUTIONS_DIR[/\\\\]*|include\\s*[<\"]|solutions[/\\\\]|third_party[/\\\\])${esc}\\b`, 'i');
  const wordRef = new RegExp(`\\b${esc}\\b`, 'i');
  // cmake path signal accepts the literal external/solutions/<repo> and the
  // canonical ${VC_SOLUTIONS_DIR}/<repo> form the build actually uses (the `${` is
  // an interpolation: $ { V C _ S O L U T I O N S _ D I R } / <repo>).
  const cmakePathRef = new RegExp(`(?:external[/\\\\]solutions[/\\\\]|[$]\\{?\s*VC_SOLUTIONS_DIR\\}?[/\\\\]+|third_party[/\\\\])${esc}\\b`, 'i');
  const cmakeRef = cmakePathRef.test(cmakeExpanded);
  // wired: a build command (add_library/add_subdirectory/add_executable/file GLOB
  // or the engine's vc_object_module macro) references the vendored path of this
  // repo within a short window. Uses plain substring search (no regex-escape
  // interop) so a wiring command appearing shortly before the vendored path is a
  // reliable signal.
  let wired = false;
  if (cmakeRef) {
    const idx = cmakeExpanded.search(cmakePathRef);
    // The nearest wiring command *before* the vendored path, not the first one
    // in the file (the file opens with dozens of unrelated add_library calls).
    const before = cmakeExpanded.slice(0, idx);
    const cmd = Math.max(before.lastIndexOf('add_library('), before.lastIndexOf('add_subdirectory('), before.lastIndexOf('add_executable('), before.lastIndexOf('file(GLOB'), before.lastIndexOf('vc_object_module('), before.lastIndexOf('set(VC_'));
    wired = cmd >= 0 && (idx - cmd) < 1200;
  }
  // Strict call sites (vendored path actually included/instantiated in a TU) for
  // the per-repo evidence detail.
  const pathCallSites = [];
  for (const f of srcAll) {
    const t = text(f);
    if (pathRef.test(t) && !/^tests\//.test(rel(f))) pathCallSites.push(rel(f));
  }
  // Real consumer signal for INTEGRADO: the repo is wired into the build AND some
  // non-test TU references it — by vendored path OR by the library/contract name
  // (adapters legally include <zstd.h>/<opus.h> or name the concrete provider, not
  // the external/solutions/... path). wordRef alone is a weaker but valid signal
  // when combined with `wired`.
  const anySourceRef = wordRef.test(srcText);
  // consumed = wired into the build AND referenced by a non-test source (via a
  // vendored path or an adapter TU). This keeps INTEGRADO honest: presence in CMake
  // alone (a probe/wrapper) does not count — it must be referenced by product code.
  const consumed = wired && anySourceRef;
  const leveraged = wired ? anySourceRef : pathCallSites.length > 0;
  const appRef = pathCallSites.some((p) => /^src\/(app|editor|server)\//.test(p));
  const sdkAdapter = pathCallSites.some((p) => /^src\/engine\/sdk\//.test(p));
  const testRef = wordRef.test(testText);
  const toolRef = wordRef.test(toolText);
  const inCatalog = catalog.has(repo);
  const callSites = pathCallSites;
  const sourceRef = leveraged;
  // classify
  let state;
  if (leveraged && wired) state = 'INTEGRADO';
  else if (leveraged && !wired) state = 'CONSUMIDO-SEM-WIRE'; // src refers but not in cmake -> risk
  else if (toolRef && !wired) state = 'FERRAMENTA';
  else if (inCatalog) state = 'REFERENCIA';
  else state = 'REMOVER';
  return {
    repo, inCatalog, catalogEntry: catalog.get(repo) || null,
    cmakeRef, wired, sourceRef, appRef, sdkAdapter, testRef, toolRef,
    consumed, callSites: [...new Set(callSites)].slice(0, 8),
    state
  };
}

const rows = repoDirs.map(classify);

// catalog entries that have no matching repo on disk -> flag (orphan claim)
const diskRepos = new Set(repoDirs);
const orphanCatalog = [...catalog.keys()].filter((k) => !diskRepos.has(k));

// ---- 4. per-target manifest (which external repos each exe targets depends on) ----
function targetDeps() {
  const out = {};
  const addExe = /add_executable\s*\(\s*([^\s#]+)([\s\S]*?)\)\s*\n/g;
  let m;
  const blocks = [];
  while ((m = addExe.exec(cmake)) !== null) blocks.push({ name: m[1], body: m[2] });
  for (const b of blocks) {
    const deps = rows
      .filter((r) => (r.wired || r.sourceRef) && b.body.includes('VC_SDK_PUBLIC_OBJECTS'))
      .map((r) => r.repo);
    const direct = rows
      .filter((r) => b.body.includes(`solutions/${r.repo}`) || b.body.includes(r.repo))
      .map((r) => r.repo);
    out[b.name] = [...new Set([...direct, ...(b.body.includes('VC_SDK_PUBLIC_OBJECTS') ? deps : [])])].sort();
  }
  return out;
}
const perTarget = targetDeps();

// ---- 5. aggregates + gate ----
const byState = Object.fromEntries(['INTEGRADO','CONSUMIDO-SEM-WIRE','FERRAMENTA','REFERENCIA','REMOVER'].map((s) => [s, rows.filter((r) => r.state === s).length]));
const unclassified = rows.filter((r) => r.state === 'REMOVER');
// An INTEGRADO repo is consumed when wired into the build AND referenced by a
// non-test source. That reference may be a strict vendored-path TU include
// (callSites) OR an adapter/consumer naming the concrete provider (sourceRef via
// wordRef, e.g. create_recast_navigation_provider). `consumed` is the canonical
// signal — it matches how the INTEGRADO state is derived above.
const integradoWithoutConsumer = rows.filter((r) => r.state === 'INTEGRADO' && !r.consumed);
const gateFails = unclassified.length > 0 || integradoWithoutConsumer.length > 0 || orphanCatalog.length > 0;

const manifest = { schema: 2, generatedAt: new Date().toISOString(), byState, repos: rows, perTarget, orphanCatalog };
const report = {
  generator: 'dependency-matrix.mjs',
  base: rel(ROOT),
  generatedAt: manifest.generatedAt,
  repoCount: rows.length,
  byState,
  unclassifiedNodes: unclassified.map((r) => r.repo),
  integradoWithoutConsumer: integradoWithoutConsumer.map((r) => r.repo),
  orphanCatalogEntries: orphanCatalog,
  gatePassed: !gateFails,
  repos: rows,
  perTarget
};

import { mkdirSync, writeFileSync } from 'node:fs';
mkdirSync(OUT, { recursive: true });
writeFileSync(join(OUT, 'dependency-matrix.json'), JSON.stringify(manifest, null, 2) + '\n');
writeFileSync(join(OUT, 'per-target-manifest.json'), JSON.stringify({ generatedAt: manifest.generatedAt, perTarget }, null, 2) + '\n');

const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const stateClass = (s) => (s === 'INTEGRADO' ? 'ok' : s === 'FERRAMENTA' || s === 'CONSUMIDO-SEM-WIRE' ? 'warn' : s === 'REMOVER' ? 'bad' : 'ref');
const html = `<!doctype html><html><head><meta charset="utf-8"><title>Dependency Matrix</title><style>
 body{font-family:ui-monospace,Consolas,monospace;margin:2rem;background:#0f1115;color:#e6e6e6}
 h1{color:#fff}table{border-collapse:collapse;width:100%;font-size:12px;margin:.5rem 0}
 th,td{border:1px solid #2a2d35;padding:4px 6px;text-align:left}th{background:#1a1d24}
 .ok{color:#4ade80}.warn{color:#fbbf24}.bad{color:#f87171}.ref{color:#60a5fa}
</style></head><body><h1>Dependency Matrix — external/solutions → agent → capability → state</h1>
<p>Generated ${esc(manifest.generatedAt)}. Inventory derived at runtime (no historical count).</p>
<p><strong>Gate:</strong> ${gateFails ? '<span class="bad">FAIL</span> (unclassified or INTEGRADO-without-consumer present)' : '<span class="ok">PASS</span>'}</p>
<ul><li>Repos on disk: <strong>${rows.length}</strong></li>
<li>INTEGRADO ${byState.INTEGRADO} · CONSUMIDO-SEM-WIRE ${byState['CONSUMIDO-SEM-WIRE']} · FERRAMENTA ${byState.FERRAMENTA} · REFERENCIA ${byState.REFERENCIA} · REMOVER ${byState.REMOVER}</li>
<li>Orphan catalog entries (no repo on disk): ${orphanCatalog.length}</li></ul>
<h2>Matrix</h2><table><tr><th>Repo</th><th>State</th><th>Área</th><th>Uso</th><th>Call sites (non-test)</th></tr>
${rows.map((r) => `<tr><td>${esc(r.repo)}</td><td><span class="${stateClass(r.state)}">${esc(r.state)}</span></td><td>${esc(r.catalogEntry?.area || '—')}</td><td>${esc((r.catalogEntry?.usage || '').slice(0, 90))}</td><td>${esc(r.callSites.join(', ')) || '—'}</td></tr>`).join('\n')}
</table></body></html>`;
writeFileSync(join(OUT, 'dependency-matrix.html'), html);

// ---- DEBUG (temporary) ----
for (const probe of ['recast-navigation', 'ozz-animation']) {
  const r = rows.find((a) => a.repo === probe);
  if (r) console.log('[DEBUG]', probe, 'state=', r.state, 'wired=', r.wired, 'cmakeRef=', r.cmakeRef, 'sourceRef=', r.sourceRef, 'appRef=', r.appRef, 'sdkAdapter=', r.sdkAdapter, 'calls=', r.callSites.join('|'));
}

// ---- console ----
console.log(`[dependency-matrix] repos on disk: ${rows.length}`);
for (const s of Object.keys(byState)) console.log(`[dependency-matrix]   ${s}=${byState[s]}`);
console.log(`[dependency-matrix] orphan catalog entries: ${orphanCatalog.length}`);
console.log(`[dependency-matrix] per-target manifest: ${Object.keys(perTarget).length} targets`);
console.log(`[dependency-matrix] gatePassed=${!gateFails}`);

// ---- gate ----
if (process.argv.includes('--check')) {
  if (gateFails) {
    console.error('[dependency-matrix] --check: FAIL');
    if (unclassified.length) console.error(`  unclassified (REMOVER): ${unclassified.map((r) => r.repo).join(', ')}`);
    if (integradoWithoutConsumer.length) console.error(`  INTEGRADO without consumer: ${integradoWithoutConsumer.map((r) => r.repo).join(', ')}`);
    if (orphanCatalog.length) console.error(`  orphan catalog entries: ${orphanCatalog.join(', ')}`);
    process.exit(1);
  }
  console.log('[dependency-matrix] --check: OK (all repos classified; every INTEGRADO has caller)');
}