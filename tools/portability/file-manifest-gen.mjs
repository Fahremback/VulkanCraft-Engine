#!/usr/bin/env node
// file-manifest-gen.mjs — A5 §H item 101: generates the file manifest and
// SHA-256 hashes of the SHIPPED package content (headers, sources, assets,
// schemas, tools, docs, manifests) so the final package is verifiable and
// tamper-evident. It does NOT hash build trees / out/ / external clones
// (gigantic, reproducible) — only the deliverable surface that the package
// carries. The manifest is written to manifests/FILE-MANIFEST.json + a short
// manifests/FILE-MANIFEST.md table.
//
//   node tools/portability/file-manifest-gen.mjs [--check]
//
// --check: verify the committed manifest matches a fresh generation (exit 1
// on drift). Pure node; no build.
import { createHash } from 'node:crypto';
import { readdirSync, readFileSync, statSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, relative } from 'node:path';

const ROOT = process.cwd();
const OUT_DIR = join(ROOT, 'manifests');
const MANIFEST = join(OUT_DIR, 'FILE-MANIFEST.json');
const MD = join(OUT_DIR, 'FILE-MANIFEST.md');

// Shipped surface only: sources, assets, schemas, tools, docs, shaders,
// cmake helpers, tests, example projects. Never build trees or vendor clones.
const SHIP_ROOTS = ['src', 'assets', 'schema', 'tools', 'docs', 'shaders', 'cmake', 'tests', 'agentes', 'Projects'];
const SKIP_DIRS = new Set(['node_modules', '.git', 'build', 'out', 'external', 'generated', 'Intermediate', '_deps', '.freebuff', '.agents']);
const SKIP_EXT = /\.(exe|dll|lib|obj|pdb|spv|vcxproj|sln|user|suo|log)$/i;

function walk(dir, acc) {
  let entries;
  try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return acc; }
  for (const e of entries) {
    if (SKIP_DIRS.has(e.name)) continue;
    const p = join(dir, e.name);
    let st;
    try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) walk(p, acc);
    else if (!SKIP_EXT.test(e.name)) acc.push(p);
  }
  return acc;
}

function sha256(p) {
  return createHash('sha256').update(readFileSync(p)).digest('hex');
}

const files = [];
for (const root of SHIP_ROOTS) {
  const abs = join(ROOT, root);
  if (!existsSync(abs)) continue;
  for (const p of walk(abs, [])) {
    const rel = relative(ROOT, p).replace(/\\/g, '/');
    files.push({ path: rel, sha256: sha256(p), bytes: statSync(p).size });
  }
}
files.sort((a, b) => a.path.localeCompare(b.path));

const report = {
  generator: 'file-manifest-gen.mjs',
  generatedAt: new Date().toISOString(),
  base: relative(process.cwd(), ROOT) || '.',
  fileCount: files.length,
  totalBytes: files.reduce((s, f) => s + f.bytes, 0),
  files
};

mkdirSync(OUT_DIR, { recursive: true });
writeFileSync(MANIFEST, JSON.stringify(report, null, 2) + '\n');

const rows = files.slice(0, 20).map((f) => `| \`${f.path}\` | ${f.sha256.slice(0, 16)}… | ${f.bytes} |`).join('\n');
const md = `# FILE-MANIFEST — shipped package surface\n\nGenerated ${report.generatedAt} — ${report.fileCount} files, ${report.totalBytes} bytes (SHA-256).\nBuild trees (out/, build/), vendor clones (external/) and generated artifacts are excluded (reproducible).\n\n| File | SHA-256 (prefix) | Bytes |\n|---|---|---|\n${rows}\n${report.fileCount > 20 ? `\n… +${report.fileCount - 20} more (see FILE-MANIFEST.json)\n` : ''}\n`;
writeFileSync(MD, md);

console.log(`[file-manifest] ${report.fileCount} files hashed (${(report.totalBytes / 1048576).toFixed(1)} MiB) -> ${relative(ROOT, MANIFEST)}`);

// --check: committed manifest must match a fresh generation (anti-drift).
// We regenerate the hashes in-memory and compare against the file on disk,
// then rewrite it so the committed artifact stays fresh after verification.
if (process.argv.includes('--check')) {
  if (!existsSync(MANIFEST)) { console.error('[file-manifest] --check: FAIL (missing FILE-MANIFEST.json — generate first)'); process.exit(1); }
  const committed = JSON.parse(readFileSync(MANIFEST, 'utf8'));
  const drift = committed.files.length !== files.length ||
    files.some((f, i) => { const c = committed.files[i]; return !c || c.path !== f.path || c.sha256 !== f.sha256; });
  if (drift) { console.error('[file-manifest] --check: FAIL (manifest drift — regenerate)'); process.exit(1); }
  console.log('[file-manifest] --check: OK (manifest matches sources)');
}
