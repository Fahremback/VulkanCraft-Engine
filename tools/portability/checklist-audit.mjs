#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const root = process.cwd();
const plan = path.join(root, 'agentes', 'agente6_integracao', 'task_plan.md');
const bugs = path.join(root, 'agentes', 'agente6_integracao', 'bugs.md');
const text = fs.readFileSync(plan, 'utf8');
const unchecked = [...text.matchAll(/^\s*[-*]\s+\[([ ~])\]/gm)].length;
const stale = [...text.matchAll(/AUDITORIA\s+2026-08-27[^\n]*REABERTO/gi)].length;
const historicalOnly = [...text.matchAll(/\(20\d\d-\d\d-\d\d[^)]*\)/g)].length;
const missingRefs = [];
for (const match of text.matchAll(/`((?:tools|src|tests|docs)\/[A-Za-z0-9_.\-/]+)`/g)) {
  const ref = match[1].split(/[),;]/)[0];
  if (!fs.existsSync(path.join(root, ref)) && !ref.includes('*')) missingRefs.push(ref);
}
const openBugs = fs.existsSync(bugs)
  ? [...fs.readFileSync(bugs, 'utf8').matchAll(/\|\s*(BUG-[^|]+)\s*\|\s*(ABERTO|BLOQUEADO|PARCIAL)/gi)].map((m) => `${m[1].trim()}:${m[2].toUpperCase()}`)
  : ['bugs.md:missing'];
const report = {
  schema: 1,
  generatedAt: new Date().toISOString(),
  plan: path.relative(root, plan).replaceAll(path.sep, '/'),
  unchecked,
  staleEvidence: stale,
  historicalEvidenceMarkers: historicalOnly,
  missingReferences: [...new Set(missingRefs)],
  openBugs,
  readyForValidation: unchecked === 0 && stale === 0 && missingRefs.length === 0 && openBugs.length === 0
};
const output = process.argv[2] ?? 'out/artifacts/checklist-audit.json';
const target = path.join(root, output);
fs.mkdirSync(path.dirname(target), { recursive: true });
fs.writeFileSync(target, JSON.stringify(report, null, 2) + '\n');
console.log(`checklist-audit: ${unchecked} unchecked, ${stale} stale markers, ${missingRefs.length} missing refs, ${openBugs.length} open bugs`);
if (process.argv.includes('--strict') && !report.readyForValidation) process.exit(1);
