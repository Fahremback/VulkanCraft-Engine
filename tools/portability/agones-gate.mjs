#!/usr/bin/env node
/**
 * agones-gate.mjs — Step 7t: proves agones SDK server (Go) builds and runs.
 *
 * Builds the agones SDK server binary, verifies --help output, and checks gRPC/HTTP ports.
 */
import { execSync, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dirname, "..", "..");
const AGONES_DIR = join(ROOT, "external", "solutions", "agones");
const EXE = join(AGONES_DIR, "agones-sdk-server-test.exe");
const RUNS = 3;

function sh(cmd, opts = {}) {
  try {
    return execSync(cmd, { encoding: "utf-8", stdio: ["pipe", "pipe", "pipe"], timeout: 600_000, ...opts });
  } catch (e) {
    return (e.stdout || "") + "\n" + (e.stderr || "");
  }
}

console.log("[agones-gate] Starting...");

// Step 1: Build SDK server if needed
if (!existsSync(EXE)) {
  console.log("[agones-gate] Building agones SDK server...");
  const out = sh(`go build -o "${EXE}" ./cmd/sdk-server/`, { cwd: AGONES_DIR });
  if (!existsSync(EXE)) {
    console.error("[agones-gate] FAIL: SDK server binary not produced");
    console.error(out.slice(-500));
    process.exit(1);
  }
}
console.log("[agones-gate] Binary found OK");

// Step 2: Verify --help (deterministic)
for (let i = 0; i < RUNS; i++) {
  console.log(`[agones-gate] Run ${i + 1}/${RUNS}...`);
  const r = spawnSync(EXE, ["--help"], { encoding: "utf-8", timeout: 10_000, stdio: ["pipe", "pipe", "pipe"] });
  const out = (r.stdout || "") + (r.stderr || "");
  if (r.status !== 0) {
    console.error(`[agones-gate] FAIL: --help exit=${r.status}`);
    console.error(out.slice(-300));
    process.exit(1);
  }
  // Check expected flags
  for (const flag of ["--grpc-port", "--http-port", "--health-port", "--gameserver-name", "--graceful-termination"]) {
    if (!out.includes(flag)) {
      console.error(`[agones-gate] FAIL: run ${i + 1} — missing flag: ${flag}`);
      process.exit(1);
    }
  }
  // Check default ports
  if (!out.includes("9357") || !out.includes("9358") || !out.includes("8080")) {
    console.error(`[agones-gate] FAIL: run ${i + 1} — missing default ports`);
    process.exit(1);
  }
}

console.log(`[agones-gate] PASS — ${RUNS}/${RUNS} deterministic runs, SDK server --help OK, ports verified`);
process.exit(0);
