#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const root = process.cwd();
const contract = {
  schema: 1,
  required: [
    'commit',
    'platform',
    'startedAt',
    'finishedAt',
    'command',
    'exitCode',
    'artifacts'
  ],
  constraints: {
    platform: 'win32',
    exitCode: 0,
    artifactsMustBeRelative: true,
    artifactsMustExist: true,
    historicalLogsAreNotEvidence: true
  },
  phase: 'validation-only; do not generate until implementation checkpoints are closed'
};
// The output path is argv[2]; a flag-looking argument (e.g. `--help`/`--strict`)
// is almost always a mistaken invocation and must NOT be treated as a filename
// (that created junk files named `--help`/`--strict` at the repo root).
const output = process.argv[2] ?? 'out/artifacts/evidence-contract.json';
if (output.startsWith('-')) {
  console.error('evidence-contract: refusing to write to flag-looking output path: ' + output);
  console.error('usage: node evidence-contract.mjs [output.json]');
  process.exit(2);
}
const target = path.resolve(root, output);
fs.mkdirSync(path.dirname(target), { recursive: true });
fs.writeFileSync(target, JSON.stringify(contract, null, 2) + '\n');
console.log(`evidence-contract: wrote ${output}`);
