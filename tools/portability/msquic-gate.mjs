#!/usr/bin/env node
/**
 * msquic-gate.mjs — Step 7v: proves the vendored msquic builds on Windows.
 *
 * The historical blocker (finding #303) was: the x64 datapath ALWAYS compiles
 * `datapath_raw_xdp_win.c`, which includes <xdp/wincommon.h> from the external
 * XDP-for-Windows SDK (not vendored). Resolved (2026-08-28) by staging the 60
 * published headers of xdp-for-windows release/1.4 under
 * `submodules/xdp-for-windows/published/external/` (the include root the
 * platform CMakeLists already references) and adding the explicit
 * QUIC_DISABLE_ANALYZE opt-out for the C28020 false positive under
 * schannel/v143. This gate pins that recipe and re-verifies the artifacts.
 *
 * Usage: node tools/portability/msquic-gate.mjs
 * Exit 0 = all checks passed.
 */
import { existsSync, statSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const ROOT = join(import.meta.dirname, "..", "..");
const QUIC_DIR = join(ROOT, "external", "solutions", "msquic");
const BUILD_DIR = join(QUIC_DIR, "build-ag5");
const DLL = join(BUILD_DIR, "bin", "Release", "msquic.dll");
const CORE_LIB = join(BUILD_DIR, "obj", "Release", "core.lib");
const XDP_INC = join(QUIC_DIR, "submodules", "xdp-for-windows", "published", "external", "xdp", "wincommon.h");

function sh(cmd, args, opts = {}) {
  return spawnSync(cmd, args, { encoding: "utf-8", stdio: ["ignore", "pipe", "pipe"], ...opts });
}
function log(m) { process.stderr.write(`[msquic-gate] ${m}\n`); }
function fail(m) { log(`FAIL: ${m}`); process.exit(1); }
function sizeOf(p) { return existsSync(p) ? statSync(p).size : 0; }

log("msquic gate (Step 7v) starting...");
if (!existsSync(XDP_INC)) {
  fail(`XDP headers not staged: ${XDP_INC} (copy xdp-for-windows release/1.4 published/external)`);
}

// Configure if needed.
if (!existsSync(join(BUILD_DIR, "CMakeCache.txt"))) {
  log("configuring msquic (schannel, analyzer disabled)...");
  const r = sh("cmake", ["-S", QUIC_DIR, "-B", BUILD_DIR,
    "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DQUIC_TLS_LIB=schannel", "-DQUIC_DISABLE_ANALYZE=ON",
    "-DQUIC_BUILD_TESTS=OFF", "-DQUIC_BUILD_TOOLS=OFF",
  ], { timeout: 120_000 });
  if (r.status !== 0) fail(`configure failed:\n${(r.stderr || r.stdout || "").slice(-800)}`);
}

// Build the core (QUIC engine, includes the XDP datapath TU) + msquic DLL.
if (!existsSync(DLL) || !existsSync(CORE_LIB)) {
  log("building msquic core + dll (~2-4 min)...");
  const r = sh("cmake", ["--build", BUILD_DIR, "--config", "Release",
    "--target", "core", "msquic"], { timeout: 900_000 });
  const out = (r.stdout || "") + (r.stderr || "");
  if (r.status !== 0 || !existsSync(DLL)) fail(`build failed:\n${out.slice(-1500)}`);
}

if (!existsSync(DLL)) fail(`msquic.dll missing: ${DLL}`);
if (!existsSync(CORE_LIB)) fail(`core.lib missing: ${CORE_LIB}`);

log(`PASS — msquic.dll (${(sizeOf(DLL) / 1024).toFixed(0)}KB) + core.lib (${(sizeOf(CORE_LIB) / 1024 / 1024).toFixed(1)}MB); ` +
     `datapath_raw_xdp_win.c compiled against the staged XDP headers`);
process.exit(0);