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
const output = process.argv[2] ?? 'out/artifacts/evidence-contract.json';
const target = path.resolve(root, output);
fs.mkdirSync(path.dirname(target), { recursive: true });
fs.writeFileSync(target, JSON.stringify(contract, null, 2) + '\n');
console.log(`evidence-contract: wrote ${output}`);
