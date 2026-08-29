#!/usr/bin/env node
// ci-matrix.mjs — Agent 6 §5.80: CI matrix for Windows
// Runs: configure → build → unit tests → voxel tests → consumer gate → package
import { execSync } from 'child_process';
import { existsSync, mkdirSync } from 'fs';

const platform = process.platform === 'win32' ? 'windows' : 'linux';
const preset = process.argv[2] || 'release';
const buildDir = process.env.VC_BUILD_DIR || 'out/agent-6';

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

// Static integration audit is intentionally the only pre-build step here.
// Build/test/package execution remains the final validation phase.
log('Step 0: Integration lint');
run('node tools/portability/integration-lint.mjs');
log('Step 0a: Checklist audit (must be clean before final validation)');
run('node tools/portability/checklist-audit.mjs --strict');

// Step 1: Configure — the canonical build/ tree (not the out/<preset> tree):
// every downstream step (fast-gate ctest, external-consumer/moved-prefix/
// debug-release installs) resolves against build/, so configuring the preset's
// out/ tree here would BUILD a tree the rest of the matrix never consumes
// (preset 'release' -> out/release, Ninja; build/ is the MSVC multi-config
// tree the gates install from).
log('Step 1: Configure (build/)');
if (!existsSync(buildDir)) mkdirSync(buildDir, { recursive: true });
run(`cmake -S . -B ${buildDir}`);

// Step 2: Build
log('Step 2: Build (build/ Release)');
run(`cmake --build ${buildDir} --config Release`);

// Step 3: Unit tests (fast gate)
log('Step 3: Unit tests');
run('node tools/portability/fast-gate.mjs unit', { env: { ...process.env, VC_BUILD_DIR: buildDir } });

// Step 4: Voxel tests
log('Step 4: Voxel tests');
run('node tools/portability/fast-gate.mjs voxel', { env: { ...process.env, VC_BUILD_DIR: buildDir } });

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

// task_plan Agente 5 §2 item 2 ("gerar bindings"): the bindings generator
// FAILS (exit 1) if any semantic tool lacks a C++ contract, CLI path or
// schema — the mechanical proof that the binding surface is complete (single
// source: semanticToolDefinitions + registry schemas + public headers).
log('Step 7f2: Generated bindings (BINDINGS_GENERATED.md)');
run('node tools/portability/bindings-gen.mjs');

// task_plan Agente 5 §1 item 5 ("namespaces"): the hard invariant from
// bugs.md B-219-2 (no namespace-less public header) is now ENFORCED — the
// gate FAILS on any header without a namespace and emits the canonical map
// (docs/NAMESPACE_CANONICAL.md) as the §2 codegen input.
log('Step 7f3: Namespace gate (canonical map, no namespace-less header)');
run('node tools/sdk/namespace-gate.mjs --write docs/NAMESPACE_CANONICAL.md');

// task_plan Agente 5 §7 (immer): the vendored header-only library must be
// USABLE — the probe compiles+runs against the vendored headers (vector /
// flex_vector / box / atom), proving no CMake wiring is required to consume
// it (persistent structures for undo/snapshots/timeline).
log('Step 7f4: Vendored immer probe (header-only usable without wiring)');
run('node tools/portability/immer-gate.mjs');

// task_plan Agente 5 §7 (sqlite): the vendored SQLite must be USABLE — the
// probe compiles+runs against the generated amalgamation (open/exec/prepare/
// bind/step on :memory:), proving the vendored source tree is consumable
// (P2 local catalogs/metadata). The amalgamation is generated by the
// canonical chain from the vendored tree (lemon/mkopcode/mksqlite3c).
log('Step 7f5: Vendored sqlite probe (amalgamation usable without wiring)');
run('node tools/portability/sqlite-gate.mjs');

// task_plan Agente 5 §7 (libsodium): the vendored crypto lib must be USABLE —
// the probe compiles+runs against the MSVC static lib (ed25519 keypair/sign/
// verify/tamper-reject), proving the real crypto backend for the
// ISignatureVerifier pluggable contract (§6 item 5) without CMake wiring.
log('Step 7f6: Vendored libsodium probe (ed25519 sign/verify usable)');
run('node tools/portability/libsodium-gate.mjs');

// task_plan Agente 5 §6 item 5: the REAL wiring — IPackageManager contract
// with the concrete SodiumSignatureVerifier (libsodium ed25519) end-to-end:
// manifest register, real sign, verify, install with valid sigs (deps
// verified), tamper/wrong-content/malformed rejected all-or-nothing.
log('Step 7f7: IPackageManager + libsodium E2E (real signature wiring)');
run('node tools/portability/package-manager-sodium-gate.mjs');

// task_plan Agente 5 §7 (curl): the vendored libcurl must be USABLE — the
// probe compiles+runs against the MSVC static lib (URL API / escape /
// malformed-reject / Schannel version), proving the vendored tree is
// consumable for HTTP(S) without CMake wiring.
log('Step 7f8: Vendored curl probe (URL API + escape usable)');
run('node tools/portability/curl-gate.mjs');

// task_plan Agente 5 §7 (python-tuf): the vendored TUF must be USABLE — the
// probe runs the REAL secure-update flow against the vendored tree (repo
// authoring with ed25519 + root threshold, consistent snapshots, client
// bootstrap/refresh/download with hash verification, tampered target /
// corrupted timestamp / rollback all rejected), proving the vendored tree is
// consumable for secure updates (the model behind §6 item 5) without any
// engine wiring. The declared dep securesystemslib[crypto] is installed
// in-tree (.venv-deps) — nothing global.
log('Step 7f9: Vendored python-tuf probe (secure-update flow usable)');
run('node tools/portability/tuf-gate.mjs');

// task_plan Agente 5 §7 (luau): the vendored scripting runtime must be
// USABLE — the probe compiles+runs against the MSVC static libs (source →
// bytecode via luau_compile, VM load/run via luau_load/lua_pcall, runtime
// errors propagated, table/function C API), proving the vendored tree is
// consumable for the §3 sandboxed scripting runtime without engine CMake
// wiring.
log('Step 7f10: Vendored luau probe (compile + VM run usable)');
run('node tools/portability/luau-gate.mjs');

// task_plan Agente 5 §3 item 6: the REAL wiring — the ILuauSandbox contract
// (engine/scripting/) with the concrete LuauRunner (vendored Luau #302)
// end-to-end: attach real runner, evaluate a real Luau script through the
// policy adapter, real JSON value back, real runtime errors with stable tags,
// sandbox by construction (io/require absent), bit-exact persistence.
log('Step 7f11: ILuauSandbox + vendored Luau E2E (real scripting wiring)');
run('node tools/portability/luau-sandbox-e2e-gate.mjs');

// task_plan Agente 5 §7 (sentry-native): the vendored crash reporting lib
// must be USABLE — the probe compiles+runs against the MSVC static lib
// (inproc backend: init, custom transport capturing the envelope with no
// network, event capture with tags/release, shutdown flush), proving the
// vendored tree is consumable for crash reporting without engine wiring
// (the real backend behind the IObservability ISink, §6 item 6).
log('Step 7f12: Vendored sentry-native probe (crash reporting usable)');
run('node tools/portability/sentry-gate.mjs');

// task_plan Agente 5 §6 item 6: the REAL wiring — IObservability contract
// with the concrete SentrySink (vendored sentry-native #306) end-to-end:
// attach the replaceable sink, route logs/spans/counters as real sentry
// events (custom transport, no network), opt-in respected, crash context
// preserved, bit-exact persistence.
log('Step 7f13: IObservability + sentry-native E2E (real crash-reporting wiring)');
run('node tools/portability/observability-sentry-e2e-gate.mjs');

// Step 7f14: opentelemetry-cpp (OTLP file exporter — trace+log+metric)
log('Step 7f14: Vendored opentelemetry-cpp probe (OTLP file exporter usable)');
run('node tools/portability/otel-gate.mjs');

log('Step 7g: Aggregate §11 platform gate');
run('node tools/portability/platform-gate.mjs');

log('Step 7h: One-shot swarm health check');
run('node tools/portability/health-check.mjs');

log('Step 7i: Freshness gate (binaries/manifests newer than sources)');
run('node tools/portability/freshness-gate.mjs');

log('Step 7j: Repo hygiene (secrets, abs paths, stale artifacts)');
run('node tools/portability/repo-hygiene-gate.mjs');

log('Step 7k: Solution status (clonado/compilado/integrado/testado/usado)');
run('node tools/portability/solution-status.mjs');

log('Step 7l: SBOM generation (real build-graph deps + versions)');
run('node tools/portability/sbom-gen.mjs');

log('Step 7m: Build-dir usage (evidence for safe cleanup gating)');
run('node tools/portability/build-dir-usage.mjs');

log('Step 7n: ConcurrentQueue correctness gate (lock-free MPMC)');
run('node tools/portability/concurrentqueue-gate.mjs');

log('Step 7o: spdlog structured logging gate');
run('node tools/portability/spdlog-gate.mjs');

log('Step 7p: Taskflow parallelism gate');
run('node tools/portability/taskflow-gate.mjs');

log('Step 7q: Benchmark regression gate (median-of-3 vs baseline)');
run('node tools/portability/benchmark-gate.mjs', { env: { ...process.env, VC_BUILD_DIR: buildDir } });

log('Step 7r: Wasmtime C API gate (WebAssembly sandboxed runtime)');
run('node tools/portability/wasmtime-gate.mjs');

log('Step 7s: Nakama multiplayer backend gate (Go build + run)');
run('node tools/portability/nakama-gate.mjs');

log('Step 7t: Agones game server orchestration gate (Go SDK server build + run)');
run('node tools/portability/agones-gate.mjs');

log('Step 7u: game-networking-sockets gate (DLL links + public API exported)');
run('node tools/portability/gns-gate.mjs');

log('Step 7v: msquic gate (XDP datapath compiles; msquic.dll + core.lib)');
run('node tools/portability/msquic-gate.mjs');

log(`CI matrix PASSED on ${platform} with preset ${preset}`);
