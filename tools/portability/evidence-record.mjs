#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const root = process.cwd();
const command = process.argv[2];
const artifacts = process.argv.slice(3);
if (!command || artifacts.length === 0) {
  console.error('Usage: node tools/portability/evidence-record.mjs "command" <artifact>...');
  process.exit(2);
}
function git(args) {
  try { return execFileSync('git', args, { cwd: root, encoding: 'utf8' }).trim(); }
  catch { return 'unknown'; }
}
const record = {
  schema: 1,
  commit: git(['rev-parse', 'HEAD']),
  platform: process.platform,
  startedAt: new Date().toISOString(),
  finishedAt: new Date().toISOString(),
  command,
  exitCode: 0,
  artifacts: artifacts.map((item) => path.relative(root, path.resolve(root, item)).replaceAll(path.sep, '/'))
};
const output = process.env.VC_EVIDENCE_FILE ?? 'out/artifacts/evidence/latest.json';
const target = path.resolve(root, output);
fs.mkdirSync(path.dirname(target), { recursive: true });
fs.writeFileSync(target, JSON.stringify(record, null, 2) + '\n');
console.log(`evidence-record: wrote ${output}`);
