#!/usr/bin/env node
/**
 * gns-gate.mjs — Step 7u: proves the vendored game-networking-sockets builds
 * and that the public API is exported from the DLL.
 *
 * The historical blocker (finding #303) was: the build's own static lib lacked
 * the public API symbols and the DLL target failed to link on 126 unresolved
 * abseil symbols. Resolved (2026-08-28) by consuming the vendored abseil
 * (external/solutions/abseil) + the otel plugin's static protobuf, linking
 * utf8_range, and building with -DProtobuf_USE_STATIC_LIBS=ON. This gate pins
 * that recipe and re-verifies the DLL + exports every run.
 *
 * Usage: node tools/portability/gns-gate.mjs
 * Env:   VC_OTEL_PROTOBUF_BUILD  otel protobuf build dir (default C:/oteltmp/build-gate/_deps/protobuf-build)
 * Exit 0 = all checks passed.
 */
import { existsSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const ROOT = join(import.meta.dirname, "..", "..");
const GNS_DIR = join(ROOT, "external", "solutions", "game-networking-sockets");
const BUILD_DIR = join(GNS_DIR, "build-ag5");
const DLL = join(BUILD_DIR, "bin", "Release", "GameNetworkingSockets.dll");
const STATIC_LIB = join(BUILD_DIR, "src", "Release", "GameNetworkingSockets_s.lib");
const ABSL_DIR = join(ROOT, "external", "solutions", "abseil");
const PROTOBUF_BUILD = process.env.VC_OTEL_PROTOBUF_BUILD
  || "C:/oteltmp/build-gate/_deps/protobuf-build";
const KEY_EXPORTS = [
  "GameNetworkingSockets_Init",
  "SteamNetworkingSockets_LibV12",
  "SteamNetworkingUtils_LibV4",
];

function sh(cmd, args, opts = {}) {
  return spawnSync(cmd, args, { encoding: "utf-8", stdio: ["ignore", "pipe", "pipe"], ...opts });
}
function log(m) { process.stderr.write(`[gns-gate] ${m}\n`); }
function fail(m) { log(`FAIL: ${m}`); process.exit(1); }
function sizeOf(p) { return existsSync(p) ? statSync(p).size : 0; }

// Find the REAL Hostx64/x64 dumpbin by walking the MSVC tree with Node (the
// `for /r` shell form also matches directories named dumpbin.exe and is fragile
// under cmd escaping). Prefer the x64 host tool; fall back to any .exe file.
function findDumpbin() {
  const msvcRoots = [
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC",
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC",
    "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC",
  ];
  const found = [];
  const walk = (dir) => {
    let entries;
    try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      const p = join(dir, e.name);
      if (e.isDirectory()) { walk(p); continue; }
      if (/^dumpbin\.exe$/i.test(e.name) && /bin\\Hostx64\\x64$/.test(dir)) found.push(p);
      else if (/^dumpbin\.exe$/i.test(e.name)) found.push(p);
    }
  };
  for (const root of msvcRoots) if (existsSync(root)) walk(root);
  return found.find(p => /bin\\Hostx64\\x64\\dumpbin\.exe$/i.test(p)) || found[0] || null;
}

log("GNS gate (Step 7u) starting...");
if (!existsSync(ABSL_DIR)) fail(`vendored abseil missing: ${ABSL_DIR}`);
if (!existsSync(join(PROTOBUF_BUILD, "Release", "libprotobuf.lib"))) {
  fail(`otel protobuf not found at ${PROTOBUF_BUILD} (set VC_OTEL_PROTOBUF_BUILD)`);
}

// Configure if needed.
if (!existsSync(join(BUILD_DIR, "CMakeCache.txt"))) {
  log("configuring game-networking-sockets...");
  const r = sh("cmake", ["-S", GNS_DIR, "-B", BUILD_DIR,
    "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DUSE_CRYPTO=BCrypt",
    "-DBUILD_SHARED_LIB=ON", "-DBUILD_STATIC_LIB=ON",
    "-DBUILD_TESTS=OFF", "-DBUILD_EXAMPLES=OFF", "-DBUILD_TOOLS=OFF",
    "-DProtobuf_USE_STATIC_LIBS=ON",
    `-DProtobuf_INCLUDE_DIR=${PROTOBUF_BUILD}/../protobuf-src/src`,
    `-DProtobuf_LIBRARY=${PROTOBUF_BUILD}/Release/libprotobuf.lib`,
  ], { timeout: 120_000 });
  if (r.status !== 0) fail(`configure failed:\n${(r.stderr || r.stdout || "").slice(-800)}`);
}

// Build the DLL + static lib idempotently (reuses an existing successful build).
if (!existsSync(DLL) || !existsSync(STATIC_LIB)) {
  log("building GameNetworkingSockets (shared + static)...");
  const r = sh("cmake", ["--build", BUILD_DIR, "--config", "Release",
    "--target", "GameNetworkingSockets", "GameNetworkingSockets_s"], { timeout: 600_000 });
  const out = (r.stdout || "") + (r.stderr || "");
  if (r.status !== 0 || !existsSync(DLL)) fail(`build failed:\n${out.slice(-1200)}`);
}

if (!existsSync(DLL)) fail(`DLL missing: ${DLL}`);
if (!existsSync(STATIC_LIB)) fail(`static lib missing: ${STATIC_LIB}`);

// Verify exports via the real dumpbin.
const dumpbin = findDumpbin();
if (!dumpbin) fail("dumpbin not found (need MSVC BuildTools)");
log(`checking exports of ${DLL} (dumpbin)...`);
const ex = sh(dumpbin, ["/exports", DLL], { timeout: 60_000 });
const dumpOut = (ex.stdout || "") + (ex.stderr || "");
for (const sym of KEY_EXPORTS) {
  if (!dumpOut.includes(sym)) fail(`public symbol NOT exported: ${sym}`);
}
// Sanity: a real DLL with hundreds of exports, not a zero-export stub.
const exportRows = (dumpOut.match(/^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+/gm) || []).length;
if (exportRows < 50) fail(`suspiciously few exports (${exportRows})`);

log(`PASS — GameNetworkingSockets.dll (${(sizeOf(DLL) / 1024).toFixed(0)}KB, ${exportRows} exports) ` +
     `links and exports ${KEY_EXPORTS.join(", ")}; static lib (${(sizeOf(STATIC_LIB) / 1024).toFixed(0)}KB) present`);
process.exit(0);