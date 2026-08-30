#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const root = process.cwd();
const planFiles = [
  'agente1_runtime_render',
  'agente2_world_gameplay',
  'agente3_network_server',
  'agente4_editor_sdk_mcp',
  'agente5_integracao_entrega'
].map((agent) => path.join(root, 'agentes', agent, 'task_plan.md'));
const texts = planFiles.map((plan) => fs.readFileSync(plan, 'utf8'));
const text = texts.join('\n');
const unchecked = [...text.matchAll(/^\s*[-*]\s+\[([ ~])\]/gm)].length;
const stale = [...text.matchAll(/AUDITORIA\s+2026-08-27[^\n]*REABERTO/gi)].length;
const historicalOnly = [...text.matchAll(/\(20\d\d-\d\d-\d\d[^)]*\)/g)].length;
// MCP transport operation names (https://modelcontextprotocol.io) are not
// filesystem paths; a plan may reference them inside backticks for clarity
// even though no matching on-disk path exists. They are allowlisted here so
// the auditor does not treat a textual protocol reference as a broken file ref.
const mcpOperationRefs = new Set([
  'tools/call', 'tools/list', 'notifications/tools/list_changed',
  'initialize', 'notifications/initialized', 'ping',
]);
const missingRefs = [];
for (const match of text.matchAll(/`((?:tools|src|tests|docs)\/[A-Za-z0-9_./\-]+)`/g)) {
  const ref = match[1].split(/[),;]/)[0];
  if (!ref.includes('*') && !mcpOperationRefs.has(ref) && !fs.existsSync(path.join(root, ref))) missingRefs.push(ref);
}
const openBugs = planFiles.flatMap((plan) => {
  const bugs = path.join(path.dirname(plan), 'bugs.md');
  return fs.existsSync(bugs)
    ? [...fs.readFileSync(bugs, 'utf8').matchAll(/\|\s*([^|]+)\s*\|\s*(ABERTO|BLOQUEADO|PARCIAL)/gi)].map((m) => `${path.basename(path.dirname(plan))}:${m[1].trim()}:${m[2].toUpperCase()}`)
    : [`${path.basename(path.dirname(plan))}:bugs.md:missing`];
});
const report = {
  schema: 1,
  generatedAt: new Date().toISOString(),
  plans: planFiles.map((plan) => path.relative(root, plan).replaceAll(path.sep, '/')),
  unchecked,
  staleEvidence: stale,
  historicalEvidenceMarkers: historicalOnly,
  missingReferences: [...new Set(missingRefs)],
  openBugs,
  readyForValidation: unchecked === 0 && stale === 0 && missingRefs.length === 0 && openBugs.length === 0
};
// argv[2] is the output path; a flag-looking argument (e.g. `--strict`/`--help`)
// is a flag, not a filename — flags were once consumed as output paths and
// created junk files (`--help`/`--strict`) at the repo root. The first non-flag
// argument is the output path; flags are still honored via argv.includes().
const outputArg = process.argv.slice(2).find((a) => !a.startsWith('-')) ?? 'out/artifacts/checklist-audit.json';
const output = outputArg;
const target = path.join(root, output);
fs.mkdirSync(path.dirname(target), { recursive: true });
fs.writeFileSync(target, JSON.stringify(report, null, 2) + '\n');
console.log(`checklist-audit: ${unchecked} unchecked, ${stale} stale markers, ${missingRefs.length} missing refs, ${openBugs.length} open bugs`);
if (process.argv.includes('--strict') && !report.readyForValidation) process.exit(1);
