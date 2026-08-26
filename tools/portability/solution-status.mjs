#!/usr/bin/env node
// solution-status.mjs — AGENT-6 §9: classifies every external/solutions clone
// by REAL usage, distinguishing clonado / compilado / integrado / testado /
// usado. Fights false [x] marks made on mere presence. Pure node.
//
//   node tools/portability/solution-status.mjs [--json]
//
// Levels per solution:
//   clonado    — present in external/solutions only
//   compilado  — referenced by CMakeLists/cmake/ (built or globbed)
//   integrado  — linked into a target via target_link_libraries or add_*
//   testado    — has a CTest entry or test file referencing it
//   usado      — referenced by src/engine or tests source code
import { readdirSync, readFileSync, statSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const SOLUTIONS = join(ROOT, 'external', 'solutions');
const JSON_OUT = process.argv.includes('--json');

const cmakeFiles = [];
const srcFiles = [];
const testFiles = [];

function walk(dir, out, depth = 0) {
  if (depth > 5) return;
  let entries;
  try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) {
      if (/node_modules|_deps|\.git/.test(p)) continue;
      walk(p, out, depth + 1);
    } else if (/\.(cmake|txt)$/.test(e.name) && /cmake|CMakeLists/.test(p)) out.push(p);
    else if (/\.(cpp|hpp|h|cc|mjs|ts)$/.test(e.name)) out.push(p);
  }
}
walk(join(ROOT, 'cmake'), cmakeFiles);
walk(join(ROOT, 'src'), srcFiles);
walk(join(ROOT, 'tests'), testFiles);
if (existsSync(join(ROOT, 'CMakeLists.txt'))) cmakeFiles.push(join(ROOT, 'CMakeLists.txt'));

const cmakeText = cmakeFiles.map((f) => safeRead(f)).join('\n');
const srcText = srcFiles.map((f) => safeRead(f)).join('\n');
const testText = testFiles.map((f) => safeRead(f)).join('\n');

function safeRead(p) {
  try { return readFileSync(p, 'utf8'); } catch { return ''; }
}

const dirs = readdirSync(SOLUTIONS, { withFileTypes: true })
  .filter((e) => e.isDirectory())
  .map((e) => e.name)
  .sort();

const rows = [];
for (const name of dirs) {
  const lower = name.toLowerCase();
  const inCmake = new RegExp(`(?:${lower}|solutions/${name})`, 'i');
  const level =
    srcText.toLowerCase().includes(lower) ? 'usado' :
    testText.toLowerCase().includes(lower) ? 'testado' :
    /target_link_libraries|add_library|add_executable|add_subdirectory|GLOB/.test(
      (cmakeText.match(new RegExp(`[^\\n]{0,60}${lower}[^\\n]{0,60}`, 'i')) || [''])[0]) ? 'integrado' :
    inCmake.test(cmakeText) ? 'compilado' :
    'clonado';
  rows.push({ solution: name, level });
}

const counts = rows.reduce((acc, r) => ((acc[r.level] = (acc[r.level] || 0) + 1), acc), {});
if (JSON_OUT) {
  console.log(JSON.stringify({ total: rows.length, counts, rows }, null, 2));
} else {
  console.log(`# external/solutions status (${rows.length} clones)`);
  for (const c of ['usado', 'testado', 'integrado', 'compilado', 'clonado']) {
    const list = rows.filter((r) => r.level === c).map((r) => r.solution);
    if (list.length) console.log(`\n${c.toUpperCase()} (${list.length}): ${list.join(', ')}`);
  }
  console.log(`\nSummary: ${JSON.stringify(counts)}`);
}
