#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const root = process.cwd();
const directory = path.resolve(root, process.argv[2] ?? 'out/artifacts/evidence');
const output = path.resolve(root, process.argv[3] ?? 'out/artifacts/evidence/summary.json');
const failures = [];
const records = [];
if (!fs.existsSync(directory)) failures.push(`evidence directory does not exist: ${directory}`);
else {
  for (const name of fs.readdirSync(directory).filter((item) => item.endsWith('.json') && item !== path.basename(output))) {
    const file = path.join(directory, name);
    let record;
    try { record = JSON.parse(fs.readFileSync(file, 'utf8')); }
    catch (error) { failures.push(`${name}: invalid JSON (${error.message})`); continue; }
    for (const key of ['schema', 'commit', 'platform', 'startedAt', 'finishedAt', 'command', 'exitCode', 'artifacts']) {
      if (!(key in record)) failures.push(`${name}: missing ${key}`);
    }
    if (record.platform !== 'win32') failures.push(`${name}: non-Windows evidence`);
    if (record.exitCode !== 0) failures.push(`${name}: command failed`);
    if (!Array.isArray(record.artifacts) || record.artifacts.length === 0) failures.push(`${name}: no artifacts`);
    for (const artifact of record.artifacts ?? []) {
      if (typeof artifact !== 'string' || path.isAbsolute(artifact) || artifact.includes('..')) failures.push(`${name}: unsafe artifact path ${artifact}`);
      else if (!fs.existsSync(path.join(root, artifact))) failures.push(`${name}: missing artifact ${artifact}`);
    }
    records.push({ file: path.relative(root, file).replaceAll(path.sep, '/'), ...record });
  }
}
const commits = [...new Set(records.map((record) => record.commit))];
if (commits.length > 1) failures.push(`evidence spans ${commits.length} commits`);
const summary = { schema: 1, generatedAt: new Date().toISOString(), commit: commits[0] ?? null, records, failures, green: failures.length === 0 && records.length > 0 };
fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, JSON.stringify(summary, null, 2) + '\n');
if (failures.length) {
  console.error(`evidence-aggregate: FAIL (${failures.length} failures)`);
  for (const failure of failures) console.error(`- ${failure}`);
  process.exit(1);
}
console.log(`evidence-aggregate: PASS (${records.length} records, commit ${summary.commit})`);
