#!/usr/bin/env node
// namespace-gate.mjs — §1 item 5 (namespaces uniformes): the HARD invariant
// from bugs.md B-219-2 is "nenhum header namespace-less" — this gate makes that
// invariant REAL (it was asserted in docs but never enforced by any tool).
//
//   node tools/sdk/namespace-gate.mjs [--write docs/NAMESPACE_CANONICAL.md]
//
// 1. Walks src/engine/public/ and FAILS (exit 1) if any public header has no
//    `namespace` declaration (at any nesting level) — a regression in a new
//    header now breaks the gate, not the docs.
// 2. Emits the CANONICAL NAMESPACE MAP: per public header, the top-level
//    namespace(s) it declares. This is the machine-readable input for the §2
//    codegen (namespaces canônicos gerados): unifying the 30+ ad-hoc patterns
//    (`engine{}`, `procgen{}`, `Engine::Rendering{}`, `animation{}`, ...)
//    toward `engine::<domain>` is a GLOBAL breaking change that must be driven
//    by that codegen, not by hand-editing headers while 5 agents compile.
//    This gate documents the current state deterministically (no manual list)
//    and keeps the invariant hard meanwhile.
//
// Deterministic: sorted output, single source = the filesystem walk (same as
// sdk-check). Exit 0 = invariant holds + map written; 1 = a header is
// namespace-less (or a source could not be read).
import { readFileSync, writeFileSync, readdirSync, existsSync, statSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const PUBLIC = join(ROOT, 'src', 'engine', 'public');
const writeIdx = process.argv.indexOf('--write');
const WRITE = writeIdx !== -1;
// Optional explicit output path (the smoke regenerates to a temp file and
// byte-compares); default is the committed docs/NAMESPACE_CANONICAL.md.
const OUT = WRITE && process.argv[writeIdx + 1]
  ? process.argv[writeIdx + 1]
  : join(ROOT, 'docs', 'NAMESPACE_CANONICAL.md');

function walk(dir, out = []) {
  if (!existsSync(dir)) return out;
  for (const entry of readdirSync(dir)) {
    const abs = join(dir, entry);
    const st = statSync(abs);
    if (st.isDirectory()) walk(abs, out);
    else if (entry.endsWith('.hpp')) out.push(abs);
  }
  return out;
}

// Top-level namespace declarations of a header, in declaration order.
// Handles `namespace X {` and `namespace X::Y {` and `inline namespace X {`.
function topLevelNamespaces(source) {
  const names = [];
  for (const line of source.split(/\r?\n/)) {
    const m = line.match(/^\s*(?:inline\s+)?namespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{/);
    if (m) names.push(m[1]);
  }
  return names;
}

const headers = walk(PUBLIC).sort();
const violations = [];
const map = [];

for (const abs of headers) {
  const rel = abs.slice(ROOT.length + 1).replaceAll('\\', '/');
  const source = readFileSync(abs, 'utf8');
  const namespaces = topLevelNamespaces(source);
  if (namespaces.length === 0) violations.push(rel);
  map.push({ header: rel, namespaces });
}

if (violations.length > 0) {
  console.error(`[namespace-gate] FAIL — ${violations.length} public header(s) without any namespace:`);
  for (const v of violations) console.error(`  - ${v}`);
  process.exit(1);
}

if (WRITE) {
  // Canonical map: header -> top-level namespaces (current state, sorted).
  const lines = [
    '# VulkanCraft Engine — Mapa canônico de namespaces (SDK)',
    '',
    'Gerado automaticamente por `node tools/sdk/namespace-gate.mjs --write docs/NAMESPACE_CANONICAL.md` — NÃO editar à mão.',
    'Fonte única: walk de `src/engine/public/` (mesma fonte do `SDK_API_INVENTORY.md`).',
    '',
    '> **Invariante dura (bugs.md B-219-2, agora ENFORCED pelo gate)**: nenhum header público pode existir sem namespace.',
    '> O objetivo de longo prazo (§1 item 5 / §2 codegen) é unificar os padrões ad-hoc (`engine{}`, `procgen{}`,',
    '> `Engine::Rendering{}`, `animation{}`, `voxel{}`, ...) em `engine::<domain>` — breaking change global que deve',
    '> ser dirigido pelo codegen do §2, não por edição manual de headers. Este mapa documenta o estado atual',
    '> deterministicamente como insumo desse codegen.',
    '',
    '## Headers por namespace top-level',
    '',
    '| Header | Namespace(s) top-level |',
    '|---|---|',
  ];
  for (const { header, namespaces } of map) {
    lines.push(`| \`${header}\` | \`${namespaces.join('` · `')}\` |`);
  }
  writeFileSync(OUT, lines.join('\n') + '\n');
  console.log(`[namespace-gate] wrote ${map.length} headers to docs/NAMESPACE_CANONICAL.md`);
}

console.log(`[namespace-gate] PASS — ${headers.length} public headers, ${violations.length} namespace-less (invariant holds)`);
