#!/usr/bin/env node
// script-gate-fixture.mjs — self-contained test for script-gate.mjs:
// a valid graph must PASS and an invalid graph must be REFUSED all-or-nothing.
// Uses only public scripting contract shapes (create_visual_script schema).
import { writeFileSync, mkdtempSync, rmSync } from 'fs';
import { join } from 'path';
import { tmpdir } from 'os';
import { spawnSync } from 'child_process';

const ROOT = process.cwd();
const gate = join(ROOT, 'tools', 'portability', 'script-gate.mjs');
const work = mkdtempSync(join(tmpdir(), 'script-gate-fixture-'));

const valid = {
  nodes: [
    { key: 'e1', kind: 'Event' }, { key: 'log1', kind: 'Log' },
    { key: 'sc', kind: 'Scope' }, { key: 'sce', kind: 'ScopeEnd' },
    { key: 'br', kind: 'Branch' }, { key: 'r1', kind: 'Return' },
  ],
  links: [
    { from: 'e1', to: 'log1' }, { from: 'log1', to: 'sc' }, { from: 'sc', to: 'br' },
    { from: 'br', to: 'r1' }, { from: 'br', to: 'sce' },
  ],
};
const invalid = {
  nodes: [{ key: 'x1', kind: 'Nope' }, { key: 'sc', kind: 'Scope' }],
  links: [{ from: 'ghost', to: 'x1' }],
};

const validPath = join(work, 'valid.json');
const invalidPath = join(work, 'invalid.json');
writeFileSync(validPath, JSON.stringify(valid));
writeFileSync(invalidPath, JSON.stringify(invalid));

function runGate(path) {
  return spawnSync(process.execPath, [gate, path], { encoding: 'utf8', timeout: 30000, windowsHide: true });
}

const ok = runGate(validPath);
const bad = runGate(invalidPath);

let failures = 0;
if (ok.status !== 0) { failures++; console.error('valid graph should PASS but exited ' + ok.status); }
if (!/PASSED/.test(ok.stdout)) { failures++; console.error('valid graph missing PASSED marker'); }
if (bad.status !== 1) { failures++; console.error('invalid graph should exit 1 but exited ' + bad.status); }
if (!/all-or-nothing/.test(bad.stderr) || !/E-1002/.test(bad.stderr)) { failures++; console.error('invalid graph missing refusal detail'); }

try { rmSync(work, { recursive: true, force: true }); } catch { /* ignore */ }

if (failures) {
  console.error(`SCRIPT GATE FIXTURE FAILED (${failures})`);
  process.exit(1);
}
console.log('SCRIPT GATE FIXTURE PASSED — valid graph accepted, invalid graph refused all-or-nothing.');
