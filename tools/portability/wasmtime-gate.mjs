#!/usr/bin/env node
/**
 * wasmtime-gate.mjs — Step 7f15: proves wasmtime C API (Rust staticlib) is usable.
 */
import { execSync, spawnSync } from "node:child_process";
import { existsSync, readdirSync, statSync, writeFileSync, readFileSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dirname, "..", "..");
const WT_DIR = join(ROOT, "external", "solutions", "wasmtime");
const PROBE_CPP = join(ROOT, "tools", "portability", "wasmtime-probe.cpp");
const PROBE_EXE = join(ROOT, "tools", "portability", "wasmtime-probe.exe");
const BUILD_DIR = join(WT_DIR, "target", "release");
const RUNS = 3;

function sh(cmd) {
  try {
    return execSync(cmd, { encoding: "utf-8", stdio: ["pipe", "pipe", "pipe"], timeout: 600_000 });
  } catch (e) {
    return (e.stdout || "") + "\n" + (e.stderr || "");
  }
}

function findGeneratedInc() {
  try {
    for (const d of readdirSync(join(BUILD_DIR, "build"))) {
      if (d.startsWith("wasmtime-c-api-impl-")) {
        const inc = join(BUILD_DIR, "build", d, "out", "include");
        if (existsSync(join(inc, "wasmtime", "conf.h"))) return inc;
      }
    }
  } catch {}
  return null;
}

console.log("[wasmtime-gate] Starting...");

const cApiLib = join(BUILD_DIR, "deps", "wasmtime_c_api.lib");
if (!existsSync(cApiLib)) {
  console.error("[wasmtime-gate] FAIL: wasmtime_c_api.lib not found");
  process.exit(1);
}

const generatedInc = findGeneratedInc();
if (!generatedInc) {
  console.error("[wasmtime-gate] FAIL: generated includes not found");
  process.exit(1);
}

// Collect all static libs
const allLibs = [];
function collectLibs(dir) {
  try {
    for (const f of readdirSync(dir)) {
      const p = join(dir, f);
      if (statSync(p).isDirectory()) collectLibs(p);
      else if (f.endsWith(".lib") && !f.endsWith(".dll.lib")) allLibs.push(p);
    }
  } catch {}
}
collectLibs(join(BUILD_DIR, "deps"));
collectLibs(join(BUILD_DIR, "build"));

const systemLibs = ["ws2_32.lib","advapi32.lib","bcrypt.lib","ntdll.lib","userenv.lib","kernel32.lib","shell32.lib","ole32.lib","rpcrt4.lib","shlwapi.lib"];
const rspPath = join(WT_DIR, "_wt-libs.rsp");
writeFileSync(rspPath, [...allLibs, ...systemLibs].join("\n"));

const incDir = join(WT_DIR, "crates", "c-api", "include");
const objPath = join(WT_DIR, "_wt-probe.obj");
const batPath = join(WT_DIR, "_wt-build.bat");
function p(path) { return path.replace(/\//g, "\\"); }

writeFileSync(batPath, [
  '@echo off',
  'call "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul 2>&1',
  `cd /d "${p(WT_DIR)}"`,
  `cl /MT /O2 /EHsc /std:c++17 /DWASM_API_EXTERN= /I"${p(incDir)}" /I"${p(generatedInc)}" /c /Fo"${p(objPath)}" "${p(PROBE_CPP)}"`,
  `link /nologo /FORCE:MULTIPLE /out:"${p(PROBE_EXE)}" "${p(objPath)}" @"${p(rspPath)}"`,
  'echo BUILD_RC=%ERRORLEVEL%',
].join("\r\n"));

console.log("[wasmtime-gate] Building probe...");
const buildOut = sh(`cmd //c "${p(batPath)}" 2>&1`);
if (buildOut.includes("BUILD_RC=2") || buildOut.includes("error LNK")) {
  console.error("[wasmtime-gate] FAIL: compile/link");
  console.error(buildOut.slice(-800));
  process.exit(1);
}
if (!existsSync(PROBE_EXE)) {
  console.error("[wasmtime-gate] FAIL: probe exe not found");
  process.exit(1);
}
console.log("[wasmtime-gate] Probe built OK");

// Run deterministically — use spawnSync directly to capture output
const expected = [
  "RESULT:engine_create:OK",
  "RESULT:store_create:OK",
  "RESULT:wat2wasm_add:OK",
  "RESULT:module_compile_add:OK",
  "RESULT:instance_create_add:OK",
  "RESULT:export_count:1",
  "RESULT:func_param_arity:2",
  "RESULT:func_result_arity:1",
  "RESULT:func_call_7_3:10",
  "RESULT:wat2wasm_42:OK",
  "RESULT:module_compile_42:OK",
  "RESULT:func_call_42:OK",
];

for (let i = 0; i < RUNS; i++) {
  console.log(`[wasmtime-gate] Run ${i + 1}/${RUNS}...`);
  const r = spawnSync(PROBE_EXE, [], {
    encoding: "utf-8",
    timeout: 30_000,
    cwd: join(ROOT, "tools", "portability"),
    stdio: ["pipe", "pipe", "pipe"],
  });
  const out = (r.stdout || "") + "\n" + (r.stderr || "");
  if (!out.includes("19/19 passed")) {
    console.error(`[wasmtime-gate] FAIL: run ${i + 1} (exit=${r.status})`);
    console.error(out.slice(-500));
    process.exit(1);
  }
  const results = out.split("\n").filter((l) => l.trim().startsWith("RESULT:"));
  for (const exp of expected) {
    if (!results.some((r) => r.trim() === exp)) {
      console.error(`[wasmtime-gate] FAIL: run ${i + 1} — missing: ${exp}`);
      process.exit(1);
    }
  }
}

console.log(`[wasmtime-gate] PASS — ${RUNS}/${RUNS} deterministic runs, 19/19 checks each`);
process.exit(0);
