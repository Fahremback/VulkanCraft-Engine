#!/usr/bin/env node
// ci-lint.mjs — AGENT-6 §11 gate: validates .github/workflows/ci.yml without
// external deps (catches the BUG-015 failure class: unquoted ': ' inside a
// scalar name breaks YAML parse; tabs are invalid YAML; referenced gates must
// exist). Pure node, no npx/network needed, so it runs in CI itself.
//
//   node tools/portability/ci-lint.mjs
//
// Exit 0 = OK. Exit 1 = defect found (all-or-nothing, no silent no-op).
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const CI = join(ROOT, '.github', 'workflows', 'ci.yml');
const problems = [];

// 1. File must exist.
if (!existsSync(CI)) {
  problems.push('missing .github/workflows/ci.yml');
} else {
  const text = readFileSync(CI, 'utf8');
  const lines = text.split(/\r?\n/);

  // 2. No tabs anywhere (YAML forbids tabs for indentation).
  lines.forEach((l, i) => {
    if (l.includes('\t')) problems.push(`line ${i + 1}: tab character (invalid YAML)`);
  });

  // 3. Every `- name:` scalar containing ': ' must be quoted (BUG-015 class:
  //    unquoted ': ' inside the value makes YAML parse it as a nested mapping).
  lines.forEach((l, i) => {
    const m = l.match(/^\s*-\s*name:\s*(.+)$/);
    if (!m) return;
    const v = m[1].trim();
    if (v.includes(': ') && !(v.startsWith('"') || v.startsWith("'"))) {
      problems.push(`line ${i + 1}: unquoted ': ' inside name scalar (BUG-015 class): ${v.slice(0, 60)}...`);
    }
  });

  // 4. Every referenced *.mjs gate must exist on disk.
  const refs = [...text.matchAll(/([a-zA-Z0-9_-]+\.mjs)/g)].map((mm) => mm[1]);
  const uniq = [...new Set(refs)];
  uniq.forEach((f) => {
    if (!existsSync(join(ROOT, 'tools', 'portability', f)) && !existsSync(join(ROOT, f))) {
      problems.push(`referenced gate missing: ${f}`);
    }
  });

  // 5. Windows is the supported certification platform. Linux is intentionally
  // not part of this product gate.
  if (!/windows-latest/.test(text)) problems.push('no windows job');
  if (!/platform-gate\.mjs/.test(text)) problems.push('platform-gate.mjs not wired into CI');
}

if (problems.length) {
  console.error('[ci-lint] FAIL:');
  problems.forEach((p) => console.error(`  - ${p}`));
  process.exit(1);
}
console.log(`[ci-lint] PASS — ${CI} valid YAML (no tabs, quoted names), gates exist, Windows job wired.`);
