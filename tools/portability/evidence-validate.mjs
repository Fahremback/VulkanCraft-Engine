#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const root = process.cwd();
const input = process.argv[2];
if (!input) {
  console.error('Usage: node tools/portability/evidence-validate.mjs <evidence.json>');
  process.exit(2);
}
const file = path.resolve(root, input);
const failures = [];
let record;
try { record = JSON.parse(fs.readFileSync(file, 'utf8')); }
catch (error) { console.error(`evidence-validate: invalid JSON: ${error.message}`); process.exit(1); }
for (const key of ['commit', 'platform', 'startedAt', 'finishedAt', 'command', 'exitCode', 'artifacts']) {
  if (!(key in record)) failures.push(`missing ${key}`);
}
if (record.platform !== 'win32') failures.push(`platform must be win32, got ${record.platform}`);
if (record.exitCode !== 0) failures.push(`exitCode must be 0, got ${record.exitCode}`);
if (!/^[0-9a-f]{7,64}$/i.test(String(record.commit ?? ''))) failures.push('commit must be a git hash');
if (!Array.isArray(record.artifacts) || record.artifacts.length === 0) failures.push('artifacts must be a non-empty array');
for (const artifact of record.artifacts ?? []) {
  if (typeof artifact !== 'string' || path.isAbsolute(artifact) || artifact.includes('..')) failures.push(`artifact must be relative: ${artifact}`);
  else if (!fs.existsSync(path.join(root, artifact))) failures.push(`artifact does not exist: ${artifact}`);
}
if (failures.length) {
  console.error('evidence-validate: FAIL');
  for (const failure of failures) console.error(`- ${failure}`);
  process.exit(1);
}
console.log(`evidence-validate: PASS (${record.artifacts.length} artifacts, commit ${record.commit})`);
