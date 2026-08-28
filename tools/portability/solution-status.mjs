#!/usr/bin/env node
// Classifies external solutions by observable repository evidence.
// This report is intentionally static; execution gates remain authoritative.
import { readdirSync, readFileSync, existsSync } from 'node:fs';
import { join, relative } from 'node:path';

const ROOT = process.cwd();
const SOLUTIONS = join(ROOT, 'external', 'solutions');
const json = process.argv.includes('--json');

function walk(dir, result = []) {
  if (!existsSync(dir)) return result;
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const file = join(dir, entry.name);
    if (entry.isDirectory()) {
      if (!/node_modules|_deps|\.git/.test(file)) walk(file, result);
    } else if (/\.(cmake|txt|cpp|cc|c|hpp|h|mjs|json)$/.test(entry.name)) {
      result.push(file);
    }
  }
  return result;
}
function text(files) {
  return files.map((file) => { try { return readFileSync(file, 'utf8'); } catch { return ''; } }).join('\n');
}
const cmakeFiles = [...walk(join(ROOT, 'cmake')), ...walk(ROOT).filter((f) => /CMakeLists\.txt$/.test(f))];
const sourceFiles = walk(join(ROOT, 'src'));
const testFiles = walk(join(ROOT, 'tests'));
const toolFiles = walk(join(ROOT, 'tools'));
const cmake = text(cmakeFiles);
const source = text(sourceFiles);
const tests = text(testFiles);
const tools = text(toolFiles);
const solutionEvidence = new Map([
  ['concurrentqueue', { source: /concurrentqueue/i }],
  ['spdlog', { source: /spdlog/i }],
  ['mimalloc', { source: /mimalloc/i }],
  ['taskflow', { source: /taskflow/i }],
  ['tracy', { source: /tracy/i }],
  ['google-benchmark', { source: /benchmark/i }],
  ['googletest', { source: /gtest|googletest/i }],
  ['fuzztest', { source: /fuzz/i }],
  ['vcpkg', { source: /vcpkg/i }]
]);

const rows = readdirSync(SOLUTIONS, { withFileTypes: true })
  .filter((entry) => entry.isDirectory()).map((entry) => entry.name).sort()
  .map((solution) => {
    const escaped = solution.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const ref = new RegExp(`(?:${escaped}|external[/\\\\]solutions[/\\\\]${escaped})`, 'i');
    const cmakeRef = ref.test(cmake);
    const evidence = solutionEvidence.get(solution);
    const sourceRef = ref.test(source) || (evidence?.source?.test(source) ?? false);
    const testRef = ref.test(tests) || (evidence?.source?.test(tests) ?? false);
    const toolRef = ref.test(tools) || (evidence?.source?.test(tools) ?? false);
    const configured = cmakeRef && /add_(?:library|executable|subdirectory)|target_link_libraries|FetchContent/i.test(cmake);
    const tested = testRef || toolRef;
    const used = sourceRef;
    const level = used ? 'usado' : tested ? 'testado' : configured ? 'integrado' : cmakeRef ? 'compilado' : 'clonado';
    return { solution, level, evidence: { cmake: cmakeRef, source: sourceRef, tests: testRef, tooling: toolRef } };
  });
const counts = Object.fromEntries([...new Set(rows.map((row) => row.level))].map((level) => [level, rows.filter((row) => row.level === level).length]));
const report = { generatedAt: new Date().toISOString(), total: rows.length, counts, rows };
if (json) console.log(JSON.stringify(report, null, 2));
else {
  console.log(`# external/solutions status (${rows.length} clones)`);
  for (const level of ['usado', 'testado', 'integrado', 'compilado', 'clonado']) {
    const list = rows.filter((row) => row.level === level).map((row) => row.solution);
    if (list.length) console.log(`\n${level.toUpperCase()} (${list.length}): ${list.join(', ')}`);
  }
  console.log(`\nSummary: ${JSON.stringify(counts)}`);
}
