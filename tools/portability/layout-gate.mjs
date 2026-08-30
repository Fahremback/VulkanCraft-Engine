#!/usr/bin/env node
// layout-gate.mjs — A5 §E (organização do repositório): the engine root may
// contain ONLY justified top-level entries. Any new file/dir at the root that
// is not allowlisted fails the gate, so new junk (accidental outputs, logs,
// stray binaries, scratch files) cannot re-accumulate. The allowlist is the
// canonical layout; entries are grouped by reason so a reviewer can see why
// each one is justified.
//
//   node tools/portability/layout-gate.mjs
//
// Exit 0 when every top-level entry is justified (allowlisted or gitignored),
// exit 1 with evidence otherwise. Pure node; no build.
import { readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const ALLOWED = new Map([
  // Source/build roots and config
  ['.github', 'CI workflows'],
  ['.gitignore', 'repo hygiene'],
  ['AGENTS.md', 'agent instructions'],
  ['CMakeLists.txt', 'build definition'],
  ['CMakePresets.json', 'CMake presets'],
  ['README.md', 'documentation'],
  ['src', 'engine source'],
  ['tests', 'test sources'],
  ['third_party', 'vendored in-tree libraries'],
  ['cmake', 'CMake helpers (pch.hpp, TestLabels.cmake)'],
  ['shaders', 'shader sources'],
  ['schema', 'schemas'],
  ['tools', 'build/portability/MCP tooling'],
  ['agentes', 'agent plans (authority)'],
  ['docs', 'technical documentation'],
  ['external', 'pinned solution clones (external/solutions)'],
  ['assets', 'runtime assets'],
  ['Projects', 'example/showcase projects'],
  ['examples', 'example consumer projects'],
  ['manifests', 'SBOM / manifests'],
  ['vcpkg.json', 'vcpkg manifest'],
  // Coordination helpers (documented entry points)
  ['ABRIR_COORDENADOR_AGENTES.bat', 'coordinator launcher'],
  ['MONITORAR_PROGRESSO.bat', 'progress monitor launcher'],
  ['atualizar_github.bat', 'publish/sync script'],
  // Root-level canonical docs/status that are the current single source
  ['task_plan.md', 'master plan (single source of execution)'],
  ['progress.md', 'progress status'],
  ['findings.md', 'technical findings'],
]);

// Entries that are allowed to exist only transiently (gitignored build state,
// generated output, local captures) — presence is tolerated, absence is fine.
// These are excluded from the FAIL set; they are reproducible.
const TRANSIENT = new Set(['build', 'out', 'generated', 'screenshots', '.sentry-native']);

function walkTop(dir) {
  const problems = [];
  let entries;
  try { entries = readdirSync(dir, { withFileTypes: true }); } catch (e) { return []; }
  for (const e of entries) {
    if (e.name === '.' || e.name === '..') continue;
    if (ALLOWED.has(e.name)) continue;
    if (TRANSIENT.has(e.name)) continue;
    problems.push(e.name);
  }
  return problems;
}

const problems = walkTop(ROOT);
if (problems.length) {
  console.error('[layout-gate] FAIL — engine root has non-allowlisted top-level entries:');
  for (const p of problems) console.error(`  - ${p}`);
  console.error('[layout-gate] Add intentional entries to the ALLOWED map with a justification,');
  console.error('[layout-gate] or remove junk. Transient build state (build/, out/, generated/,');
  console.error('[layout-gate] screenshots/, .sentry-native/) is tolerated and reproduced.');
  process.exit(1);
}
console.log('[layout-gate] PASS — every top-level entry is justified (allowlist) or transient (gitignored build state).');
