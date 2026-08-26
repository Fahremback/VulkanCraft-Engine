#!/usr/bin/env node
// ci-matrix.mjs — Agent 6 §5.80: CI matrix for Windows/Linux
// Runs: configure → build → unit tests → voxel tests → consumer gate → package
import { execSync } from 'child_process';
import { existsSync, mkdirSync } from 'fs';

const platform = process.platform === 'win32' ? 'windows' : 'linux';
const preset = process.argv[2] || 'release';

function log(msg) { console.log(`[ci-matrix] ${msg}`); }
function fail(msg) { console.error(`[ci-matrix] FAIL: ${msg}`); process.exit(1); }
function run(cmd, opts = {}) {
    try {
        return execSync(cmd, { encoding: 'utf8', timeout: 600000, ...opts });
    } catch (e) {
        fail(`${cmd} failed: ${e.message}`);
    }
}

log(`Platform: ${platform}, Preset: ${preset}`);

// Step 1: Configure — the canonical build/ tree (not the out/<preset> tree):
// every downstream step (fast-gate ctest, external-consumer/moved-prefix/
// debug-release installs) resolves against build/, so configuring the preset's
// out/ tree here would BUILD a tree the rest of the matrix never consumes
// (preset 'release' -> out/release, Ninja; build/ is the MSVC multi-config
// tree the gates install from).
log('Step 1: Configure (build/)');
if (!existsSync('build')) mkdirSync('build', { recursive: true });
run(`cmake -S . -B build`);

// Step 2: Build
log('Step 2: Build (build/ Release)');
run(`cmake --build build --config Release`);

// Step 3: Unit tests (fast gate)
log('Step 3: Unit tests');
run('node tools/portability/fast-gate.mjs unit');

// Step 4: Voxel tests
log('Step 4: Voxel tests');
run('node tools/portability/fast-gate.mjs voxel');

// Step 5: External consumer gate
log('Step 5: External consumer gate');
run('node tools/portability/external-consumer-gate.mjs');

// Step 5b: Moved-prefix gate (§8 item 2 "pasta movida": install -> move ->
// consumer builds/runs against the relocated prefix)
log('Step 5b: Moved-prefix gate');
run('node tools/portability/moved-prefix-gate.mjs');

// Step 5c: Debug/Release gate (§1 item 6 — compatibilidade debug/release):
// install BOTH configs into one prefix, then build+run the consumer in Debug
// AND Release against it. The package config must select lib/Debug/ for a
// Debug consumer and lib/ for a Release consumer (MSVC runtime compatibility).
log('Step 5c: Debug/Release gate');
run('node tools/portability/debug-release-gate.mjs');

// Step 6: Status report
log('Step 6: Status report');
run('node tools/portability/status-report.mjs');

log(`CI matrix PASSED on ${platform} with preset ${preset}`);
