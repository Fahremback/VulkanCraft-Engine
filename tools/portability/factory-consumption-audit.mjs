#!/usr/bin/env node
// factory-consumption-audit.mjs — static audit that distinguishes LINKAGE from
// CONSUMPTION for every public factory (create_* returning a unique/shared_ptr
// to an I* contract) in src/engine/public.
//
//   node tools/portability/factory-consumption-audit.mjs [--json]
import { readdirSync, readFileSync, statSync, existsSync } from 'node:fs';
import { join, relative } from 'node:path';

const root = process.cwd();
const PUBLIC = join(root, 'src', 'engine', 'public');

function walk(dir, acc = []) {
  if (!existsSync(dir)) return acc;
  let entries;
  try { entries = readdirSync(dir); } catch { return acc; }
  for (const e of entries) {
    const p = join(dir, e);
    let st;
    try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) walk(p, acc);
    else if (/\.(hpp|h|cpp|cc|cxx)$/.test(e)) acc.push(p);
  }
  return acc;
}

// ---- collect public factories -------------------------------------------------
const factoryRe = /(?:std::unique_ptr|std::shared_ptr)\s*<\s*I[A-Za-z0-9_]+(?:\s*,\s*[A-Za-z0-9_:]+)?\s*>\s+(create_[a-z0-9_]+)\s*\(/g;
const factories = [];
for (const h of walk(PUBLIC)) {
  const text = readFileSync(h, 'utf8');
  let m;
  while ((m = factoryRe.exec(text)) !== null) {
    factories.push({ name: m[1], header: relative(root, h) });
  }
}
const byName = new Map();
for (const f of factories) {
  if (!byName.has(f.name)) byName.set(f.name, { name: f.name, headers: [] });
  byName.get(f.name).headers.push(f.header);
}
const names = [...byName.keys()];

// ---- single pass over all files, one combined regex ----------------------------
// NOTE: declarations inside src/engine/public are NOT call sites — a factory
// declared in its own header must not count as consumed. Exclude that dir.
const files = ['src', 'tests', 'tools', 'examples']
  .flatMap((d) => walk(join(root, d)))
  .filter((f) => !f.startsWith(join(root, 'src', 'engine', 'public')));
// combined alternation: create_a|create_b|... escaped, word-boundary anchored
const alt = names.map((n) => n.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).join('|');
const combined = new RegExp('\\b(' + alt + ')\\s*\\(', 'g');

const hits = new Map(); // name -> [{file,testOnly,sdkInternal,app}]
for (const f of names) hits.set(f, []);
for (const file of files) {
  const rel = relative(root, file).replace(/\\/g, '/');
  let text;
  try { text = readFileSync(file, 'utf8'); } catch { continue; }
  const isTest = rel.startsWith('tests/');
  const isSdk = rel.startsWith('src/engine/sdk/');
  const isApp = /^src\/(app|editor|server)\//.test(rel);
  combined.lastIndex = 0;
  let m;
  while ((m = combined.exec(text)) !== null) {
    const name = m[1];
    hits.get(name).push({ file: rel, testOnly: isTest, sdkInternal: isSdk, app: isApp });
  }
}

// ---- classify ------------------------------------------------------------------
const rows = [];
let consumed = 0, testOnly = 0, sdkInternal = 0, declaredOnly = 0;
for (const name of names) {
  const sites = hits.get(name);
  const real = sites.filter((s) => !s.testOnly && !s.sdkInternal);
  const tests = sites.filter((s) => s.testOnly);
  const sdk = sites.filter((s) => s.sdkInternal && !s.testOnly);
  const appHit = sites.find((s) => s.app);
  let kind;
  if (real.length > 0) { kind = 'CONSUMED'; consumed++; }
  else if (tests.length > 0) { kind = 'TEST-ONLY'; testOnly++; }
  else if (sdk.length > 0) { kind = 'SDK-INTERNAL'; sdkInternal++; }
  else { kind = 'DECLARED-ONLY'; declaredOnly++; }
  rows.push({ factory: name, kind, realSites: real.length, testSites: tests.length,
              sdkSites: sdk.length, appConsumer: appHit ? appHit.file : null,
              headers: byName.get(name).headers });
}
rows.sort((a, b) => a.factory.localeCompare(b.factory));

if (process.argv.includes('--json')) {
  console.log(JSON.stringify({
    generated: new Date().toISOString(),
    factoryCount: rows.length, consumed, testOnly, sdkInternal, declaredOnly,
    rows
  }, null, 2));
} else {
  console.log(`[factory-consumption-audit] ${rows.length} public factories`);
  console.log(`  CONSUMED (app/editor/server call sites):  ${consumed}`);
  console.log(`  CONSUMED (other real call sites):         ${rows.filter((r) => r.kind === 'CONSUMED' && !r.appConsumer).length}`);
  console.log(`  TEST-ONLY (only tests/):                  ${testOnly}`);
  console.log(`  SDK-INTERNAL (own impl only):             ${sdkInternal}`);
  console.log(`  DECLARED-ONLY (no call site):             ${declaredOnly}`);
  console.log();
  console.log('CONSUMED in app/editor/server:');
  for (const r of rows.filter((r) => r.kind === 'CONSUMED' && r.appConsumer))
    console.log(`  ✓ ${r.factory}  -> ${r.appConsumer}`);
  console.log();
  console.log('DECLARED-ONLY (integration gap — no call site at all):');
  for (const r of rows.filter((r) => r.kind === 'DECLARED-ONLY'))
    console.log(`  ✗ ${r.factory}`);
}
