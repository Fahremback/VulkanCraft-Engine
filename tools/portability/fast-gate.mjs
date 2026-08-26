#!/usr/bin/env node
// fast-gate.mjs — Agent 6 §1: fast gate by label suite
// Usage: node tools/portability/fast-gate.mjs [label]
// Default label: unit (31 tests, ~1s)
// Other labels: voxel, physics, vehicle, skeleton, rendering, integration, gameplay, architecture

import { execSync } from 'child_process';
import { existsSync } from 'fs';

const label = process.argv[2] || 'unit';
const buildDir = 'build';

function log(msg) { console.log(`[fast-gate] ${msg}`); }
function fail(msg) { console.error(`[fast-gate] FAIL: ${msg}`); process.exit(1); }

// Check build dir exists
if (!existsSync(buildDir)) {
    fail(`Build directory '${buildDir}' not found. Run cmake configure first.`);
}

// Count tests with this label
log(`Running label suite: ${label}`);
let countOut;
try {
    countOut = execSync(`ctest -N -L ${label} -C Release`, { cwd: buildDir, encoding: 'utf8', timeout: 10000 });
} catch (e) {
    fail(`ctest list failed: ${e.message}`);
}
const match = countOut.match(/Total Tests:\s+(\d+)/);
const count = match ? parseInt(match[1]) : 0;
log(`Found ${count} tests with label '${label}'`);

if (count === 0) {
    fail(`No tests found with label '${label}'`);
}

// Run the tests
const start = Date.now();
let output;
try {
    output = execSync(`ctest -L ${label} -C Release --output-on-failure`, {
        cwd: buildDir,
        encoding: 'utf8',
        timeout: 300000,
        stdio: ['pipe', 'pipe', 'pipe']
    });
} catch (e) {
    output = e.stdout || '';
    const stderr = e.stderr || '';
    log(output);
    log(stderr);
    fail(`Tests failed with exit code ${e.status}`);
}

const elapsed = ((Date.now() - start) / 1000).toFixed(1);
log(output.trim().split('\n').filter(l => l.includes('Passed') || l.includes('Failed') || l.includes('Total')).join('\n'));
log(`${count} tests in ${elapsed}s — ALL PASSED`);
