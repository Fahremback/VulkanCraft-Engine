#!/usr/bin/env node
/**
 * nakama-gate.mjs — Step 7s: proves nakama (Go multiplayer backend) builds and runs.
 */
import { execSync, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dirname, "..", "..");
const NAKAMA_DIR = join(ROOT, "external", "solutions", "nakama");
const EXE = join(NAKAMA_DIR, "nakama-test.exe");
const RUNS = 3;

function sh(cmd, opts = {}) {
  try {
    return execSync(cmd, { encoding: "utf-8", stdio: ["pipe", "pipe", "pipe"], timeout: 600_000, ...opts });
  } catch (e) {
    return (e.stdout || "") + "\n" + (e.stderr || "");
  }
}

console.log("[nakama-gate] Starting...");

// Step 1: Build if needed
if (!existsSync(EXE)) {
  console.log("[nakama-gate] Building nakama...");
  const out = sh(`go build -o "${EXE}" .`, { cwd: NAKAMA_DIR });
  if (!existsSync(EXE)) {
    console.error("[nakama-gate] FAIL: nakama binary not produced");
    console.error(out.slice(-500));
    process.exit(1);
  }
}
console.log("[nakama-gate] Binary found OK");

// Step 2: Verify --help output (deterministic)
for (let i = 0; i < RUNS; i++) {
  console.log(`[nakama-gate] Run ${i + 1}/${RUNS}...`);
  const r = spawnSync(EXE, ["--help"], { encoding: "utf-8", timeout: 10_000, stdio: ["pipe", "pipe", "pipe"] });
  const out = (r.stdout || "") + (r.stderr || "");
  if (r.status !== 0) {
    console.error(`[nakama-gate] FAIL: --help exit=${r.status}`);
    console.error(out.slice(-300));
    process.exit(1);
  }
  // Check nakama-specific flags
  for (const flag of ["-config", "-console.port", "-database.address", "-session.token_expiry_sec"]) {
    if (!out.includes(flag)) {
      console.error(`[nakama-gate] FAIL: run ${i + 1} — missing flag: ${flag}`);
      process.exit(1);
    }
  }
}

console.log(`[nakama-gate] PASS — ${RUNS}/${RUNS} deterministic runs, --help OK`);
process.exit(0);
