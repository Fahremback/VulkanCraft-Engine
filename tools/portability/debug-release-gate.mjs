#!/usr/bin/env node

// Debug/Release gate (task_plan Agente 5 §1 item 6 — compatibilidade
// debug/release e fronteiras seguras de runtime).
//
// The SDK package is a set of STATIC archives. On MSVC a static archive built
// with /MD (Release) cannot be linked into a consumer built with /MDd (Debug)
// and vice-versa — LNK2038 RuntimeLibrary mismatch. The engine install rules
// now stage every SDK archive twice (non-Debug -> lib/, Debug -> lib/Debug/)
// and the package config (vulkan_craft_sdk-config.cmake) selects per consumer
// config. This gate proves the WHOLE story end-to-end:
//
//   1. Build the SDK lib set in Debug AND Release (targets: vc_sdk + the 10
//      promoted dependency archives).
//   2. Install BOTH configs into one fresh prefix (Release first, then Debug —
//      the order that used to silently clobber lib/vc_sdk.lib with /MDd).
//   3. Verify the staged tree: lib/vc_sdk.lib (Release) AND lib/Debug/vc_sdk.lib
//      (Debug) both present.
//   4. Build the external consumer in Debug against the prefix: must link the
//      Debug archives (no LNK2038) and run with its markers.
//   5. Build the SAME consumer in Release against the prefix: must link the
//      Release archives and run with its markers.
//
// Exit code 0 = both configs link + run. The engine build must exist (build/).
// Configure/install/build output is streamed to stderr.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const GATE_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(GATE_DIR, "..", "..");
// Build dir overridable (shared out-of-tree build). Default stays legacy in-tree.
const BUILD_DIR = path.join(ENGINE_ROOT, process.env.VC_BUILD_DIR ?? "build");
const KEEP = process.argv.includes("--keep");

const SDK_LIB_TARGETS = [
  "vc_sdk", "vc_zstd", "vc_blake3", "vc_navigation", "vc_fastwfc",
  "vc_meshoptimizer", "vc_xatlas", "flatbuffers", "rocksdb", "vulkan_jolt", "bullet"
];

function log(message) {
  process.stderr.write(`[debug-release-gate] ${message}${os.EOL}`);
}

function fail(message) {
  process.stderr.write(`[debug-release-gate] FAIL: ${message}${os.EOL}`);
  process.exitCode = 1;
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
    ...options
  });
  if (result.stdout) process.stderr.write(result.stdout);
  if (result.stderr) process.stderr.write(result.stderr);
  return result;
}

function main() {
  const prefix = fs.mkdtempSync(path.join(os.tmpdir(), "vc-sdk-dr-prefix-"));
  const consumerDir = fs.mkdtempSync(path.join(os.tmpdir(), "vc-sdk-dr-consumer-"));

  try {
    log(`engine root: ${ENGINE_ROOT}`);

    // 1. Build the SDK lib set in BOTH configs.
    for (const config of ["Release", "Debug"]) {
      log(`building SDK lib set (${config})`);
      const result = run("cmake", ["--build", BUILD_DIR, "--config", config, "--target", ...SDK_LIB_TARGETS]);
      if (result.status !== 0) {
        fail(`${config} build of SDK lib set exited ${result.status}`);
        return;
      }
    }

    // 2. Install both configs into one prefix — Release first, then Debug (the
    // order that used to silently clobber lib/vc_sdk.lib with the /MDd build).
    for (const config of ["Release", "Debug"]) {
      log(`cmake --install (${config})`);
      const result = run("cmake", ["--install", BUILD_DIR, "--config", config, "--prefix", prefix]);
      if (result.status !== 0) {
        fail(`cmake --install (${config}) exited ${result.status}`);
        return;
      }
    }

    // 3. Verify both variants are staged.
    for (const lib of ["vc_sdk.lib", "vc_zstd.lib", "vc_blake3.lib", "vc_navigation.lib",
      "vc_fastwfc.lib", "vc_meshoptimizer.lib", "vc_xatlas.lib", "flatbuffers.lib",
      "rocksdb.lib", "Jolt.lib", "bullet.lib"]) {
      if (!fs.existsSync(path.join(prefix, "lib", lib))) {
        fail(`staged lib/${lib} missing (Release variant)`);
        return;
      }
      if (!fs.existsSync(path.join(prefix, "lib", "Debug", lib))) {
        fail(`staged lib/Debug/${lib} missing (Debug variant)`);
        return;
      }
    }
    log("staged lib/ (Release) and lib/Debug/ (Debug) variants verified");

    // 4/5. Consumer matrix: the representative consumer built in Debug and in
    // Release against the SAME dual-config prefix.
    const consumer = path.join(consumerDir, "external-project");
    copyDirectory(path.join(ENGINE_ROOT, "tools", "external-project"), consumer);
    const configs = [
      { config: "Release", markers: ["consumer-ok registries", "consumer-ok gameplay", "consumer-ok all"], exe: "consumer.exe" },
      { config: "Debug", markers: ["consumer-ok registries", "consumer-ok gameplay", "consumer-ok all"], exe: "consumer.exe" }
    ];
    for (const spec of configs) {
      log(`configuring consumer (${spec.config}) against dual-config prefix`);
      let result = run("cmake", ["-S", consumer, "-B", path.join(consumer, `build-${spec.config}`),
        "-G", "Visual Studio 18 2026",
        `-DCMAKE_PREFIX_PATH=${prefix}`]);
      if (result.status !== 0) {
        fail(`consumer configure (${spec.config}) exited ${result.status}`);
        return;
      }
      log(`building consumer (${spec.config})`);
      result = run("cmake", ["--build", path.join(consumer, `build-${spec.config}`), "--config", spec.config]);
      if (result.status !== 0) {
        fail(`consumer build (${spec.config}) exited ${result.status} — LNK2038/mismatch would surface here`);
        return;
      }
      const consumerExe = path.join(consumer, `build-${spec.config}`, spec.config, spec.exe);
      if (!fs.existsSync(consumerExe)) {
        fail(`${spec.exe} (${spec.config}) was not produced`);
        return;
      }
      log(`running consumer (${spec.config})`);
      result = run(consumerExe, []);
      if (result.status !== 0) {
        fail(`consumer (${spec.config}) exited ${result.status}`);
        return;
      }
      const output = `${result.stdout || ""}${result.stderr || ""}`;
      for (const marker of spec.markers) {
        if (!output.includes(marker)) {
          fail(`consumer (${spec.config}) output missing marker "${marker}"`);
          return;
        }
      }
      log(`consumer (${spec.config}) output verified`);
    }

    log("DEBUG/RELEASE GATE PASSED (consumer links + runs in both configs)");
    log(`prefix: ${prefix}`);
  } catch (error) {
    fail(`unexpected error: ${error.message}`);
  } finally {
    if (!KEEP) {
      fs.rmSync(prefix, { recursive: true, force: true });
      fs.rmSync(consumerDir, { recursive: true, force: true });
      log("temp dirs cleaned");
    }
  }
}

function copyDirectory(source, target) {
  fs.mkdirSync(target, { recursive: true });
  for (const entry of fs.readdirSync(source, { withFileTypes: true })) {
    const from = path.join(source, entry.name);
    const to = path.join(target, entry.name);
    if (entry.isDirectory()) copyDirectory(from, to);
    else if (entry.isFile()) fs.copyFileSync(from, to);
  }
}

main();
