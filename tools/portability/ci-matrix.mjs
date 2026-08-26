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

// Step 5d: MCP conformance gate (§7 mcp-typescript-sdk — the server's wire
// responses must conform to the OFFICIAL MCP TypeScript SDK shapes, vendored
// at external/solutions/mcp-typescript-sdk; field sets extracted from the
// vendored schemas.ts at runtime — single source of truth).
log('Step 5d: MCP conformance gate (vendored TS SDK)');
run('node tools/portability/mcp-conformance-gate.mjs');

// Step 6: Status report
log('Step 6: Status report');
run('node tools/portability/status-report.mjs');

// Step 7: §11 platform gates (AGENT-6 — SDK/MCP platform mission)
log('Step 7: Error registry (stable codes, no orphans)');
run('node tools/portability/error-registry.mjs');

log('Step 7a: CI workflow lint (ci.yml valid YAML + gates exist — BUG-015 class)');
run('node tools/portability/ci-lint.mjs');

log('Step 7b: Contract parity (C++/CLI/MCP)');
run('node tools/portability/parity-tests.mjs game_capabilities list_game_projects');

log('Step 7c: Fuzz smoke (parsers/schemas/MCP/manifests)');
run('node tools/portability/fuzz-smoke.mjs');

log('Step 7d: Multi-client MCP concurrency + cancellation');
run('node tools/portability/concurrency-tests.mjs');

log('Step 7e: Path-with-spaces gate');
run('node tools/portability/spaces-path-gate.mjs');

log('Step 7f: Generated docs (REFERENCE_GENERATED.md)');
run('node tools/portability/gen-docs.mjs');

log('Step 7g: Aggregate §11 platform gate');
run('node tools/portability/platform-gate.mjs');

log('Step 7h: One-shot swarm health check');
run('node tools/portability/health-check.mjs');

log(`CI matrix PASSED on ${platform} with preset ${preset}`);
