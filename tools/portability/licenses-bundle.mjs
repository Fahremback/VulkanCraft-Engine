#!/usr/bin/env node
/**
 * licenses-bundle.mjs — A5 §D-57: stage redistributable LICENSE notices for the
 * dependency set the package actually ships, WITHOUT copying source trees.
 *
 * Reads manifests/SBOM.json (real build-graph usage). For every dependency
 * that is not a vendored snapshot of the engine itself (abseil artifacts,
 * eigen nested, fastnoise single-header — those carry the engine's own
 * licenses), it copies the LICENSE/COPYING/README-notice from
 * external/solutions/<name> into manifests/licenses/<name>/ and writes a
 * combined THIRD_PARTY_NOTICES.md.
 *
 * The output feeds `install(DIRECTORY manifests/licenses ...)` so the package
 * ships attribution while staying small.
 *
 * Usage:
 *   node tools/portability/licenses-bundle.mjs
 * Exit 0 = every used dep has a notice; 1 = any missing (with evidence).
 */
import { readdirSync, readFileSync, existsSync, mkdirSync, copyFileSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const SOLUTIONS = join(ROOT, 'external', 'solutions');
const SBOM_PATH = join(ROOT, 'manifests', 'SBOM.json');
const OUT_DIR = join(ROOT, 'manifests', 'licenses');

function fail(msg) { console.error(`[licenses-bundle] FAIL: ${msg}`); process.exit(1); }
function log(m) { console.log(`[licenses-bundle] ${m}`); }

if (!existsSync(SBOM_PATH)) fail(`missing ${SBOM_PATH} — run sbom-gen.mjs first`);
const sbom = JSON.parse(readFileSync(SBOM_PATH, 'utf8'));
const deps = sbom.dependencies.filter((d) => d.kind === 'external/solutions');

const NOTICE_CANDIDATES = ['LICENSE', 'LICENSE.TXT', 'LICENSE.MD', 'COPYING', 'COPYING.TXT', 'COPYING.MD', 'LICENCE', 'LICENSE.RST', 'LICENSE.APACHE', 'LICENSE_CC0', 'LICENSE_A2', 'LICENSE_A2LLVM', 'LICENSE-APACHE', 'LICENSE-MIT'];
const README_MARKER = /\b(copyright|license|permission is hereby granted|redistribution)\b/i;

function findNotice(dir) {
  let entries;
  try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return null; }
  for (const e of entries) {
    if (!e.isFile()) continue;
    const base = e.name.toUpperCase();
    if (NOTICE_CANDIDATES.some((n) => base === n)) return e.name;
  }
  // LICENSE in a nested/upstream dir (e.g. openusd/LICENSE.txt at root is
  // covered above; some repos keep it under docs/ or a submodule).
  for (const e of entries) {
    if (!e.isDirectory()) continue;
    const sub = findNotice(join(dir, e.name));
    if (sub) return `${e.name}/${sub}`;
  }
  // README.md containing license language is an acceptable fallback.
  for (const e of entries) {
    if (e.isFile() && /^readme.*\.md$/i.test(e.name)) {
      try { if (README_MARKER.test(readFileSync(join(dir, e.name), 'utf8').slice(0, 20000))) return e.name; } catch {}
    }
  }
  return null;
}

mkdirSync(OUT_DIR, { recursive: true });
const missing = [];
const bundled = [];
const rows = [];

for (const d of deps) {
  const dir = join(SOLUTIONS, d.name);
  if (d.vendored) {
    rows.push({ name: d.name, notice: 'vendored in-tree (engine-own artifact)', file: null });
    continue;
  }
  if (!existsSync(dir)) { missing.push(`${d.name}: missing on disk`); continue; }
  const notice = findNotice(dir);
  if (!notice) {
    missing.push(`${d.name}: no LICENSE/COPYING/notice found`);
    continue;
  }
  const src = join(dir, notice);
  const destDir = join(OUT_DIR, d.name);
  rmSync(destDir, { recursive: true, force: true });
  mkdirSync(destDir, { recursive: true });
  copyFileSync(src, join(destDir, notice));
  const version = d.pin ? d.pin.slice(0, 12) : (d.version || '?');
  bundled.push({ name: d.name, notice, version, origin: d.origin || '' });
  rows.push({ name: d.name, notice: `${notice} (${version})`, file: `licenses/${d.name}/${notice}` });
}

if (missing.length) {
  for (const m of missing) console.error(`  - ${m}`);
  fail(`${missing.length} dependency(ies) without a stageable license notice`);
}

const md = `# Third-party notices\n\nGerado: ${new Date().toISOString().slice(0, 10)} — pacote redistribuível.\n\nCada dependência efetivamente usada (SBOM) tem a licença stageada em \`licenses/<nome>/\`.\nLicenças completas dos vendored in-tree (abseil/eigen/fastnoise_lite) acompanham o repo da engine.\n\n| dependência | notice | pin | origem |\n|---|---|---|---|\n` +
  rows.filter((r) => r.file).map((r) => `| ${r.name} | ${r.notice} | ${(r.version || '—')} | ${r.origin || '—'} |`).join('\n') + '\n';
writeFileSync(join(ROOT, 'manifests', 'THIRD_PARTY_NOTICES.md'), md);

log(`${bundled.length} license notices staged → manifests/licenses/ + THIRD_PARTY_NOTICES.md`);
log('PASS — every used dependency has a redistributable notice.');
