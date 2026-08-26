#!/usr/bin/env node

// External-consumer gate (FALTANTES item 11 / §24 — final piece): proves the
// INSTALLED SDK is self-sufficient for a project that never sees the engine
// source tree.
//
//   1. Reconfigures the main build (picks up the new install rules) and runs
//      `cmake --install` into a fresh temporary prefix.
//   2. Verifies the staged tree: public headers + glm, the vc_sdk archive,
//      every static dependency lib, the relocatable package config, the MCP
//      server, SDK.md.
//   3. Scans the staged text files for absolute references to the engine tree.
//   4. Copies tools/external-project (a consumer whose ONLY dependency is
//      find_package(vulkan_craft_sdk CONFIG)) to a fresh directory and builds
//      + RUNS it against the install prefix — with no engine-tree reference
//      in its build tree.
//
// Usage:
//   node tools/portability/external-consumer-gate.mjs            # full gate
//   node tools/portability/external-consumer-gate.mjs --keep     # keep temp
//
// Exit code 0 = all checks passed. The engine build must exist (build/).
// Configure/install/build output is streamed to stderr.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const GATE_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(GATE_DIR, "..", "..");
const BUILD_DIR = path.join(ENGINE_ROOT, "build");
const KEEP = process.argv.includes("--keep");

const ABSOLUTE_PATH_RE = /((^|[^A-Za-z])[A-Za-z]:[\\/])|(^\\\\[^\\/])/;

function log(message) {
  process.stderr.write(`[external-consumer-gate] ${message}${os.EOL}`);
}

function fail(message) {
  process.stderr.write(`[external-consumer-gate] FAIL: ${message}${os.EOL}`);
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

function collectTextFiles(directory, relativePrefix = "") {
  const output = [];
  const walk = (dir, relative) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      if (entry.name === ".git") continue;
      const absolute = path.join(dir, entry.name);
      const rel = path.join(relative, entry.name);
      if (entry.isDirectory()) walk(absolute, rel);
      else if (entry.isFile() && isTextFile(entry.name)) output.push(rel);
    }
  };
  walk(directory, relativePrefix);
  return output;
}

function isTextFile(name) {
  const ext = path.extname(name).toLowerCase();
  return [".txt", ".md", ".json", ".cmake", ".mjs", ".js", ".hpp", ".h",
    ".cpp", ".c", ".in", ".vcxproj", ".filters", ".props", ".targets",
    ".cmake.in"].includes(ext);
}

function scanForPath(root, relativeFiles, needle) {
  const leaks = [];
  const needleNormalized = needle.replace(/\\/g, "/").toLowerCase();
  for (const file of relativeFiles) {
    const absolute = path.join(root, file);
    if (!fs.existsSync(absolute)) {
      leaks.push(`${file}: MISSING`);
      continue;
    }
    const content = fs.readFileSync(absolute, "utf8");
    for (const line of content.split(/\r?\n/)) {
      const normalized = line.replace(/\\/g, "/").toLowerCase();
      if (normalized.includes(needleNormalized)) {
        leaks.push(`${file}: ${line.trim().slice(0, 160)}`);
      }
    }
  }
  return leaks;
}

function scanForAbsolutePaths(root, relativeFiles) {
  const leaks = [];
  for (const file of relativeFiles) {
    const absolute = path.join(root, file);
    if (!fs.existsSync(absolute)) {
      leaks.push(`${file}: MISSING`);
      continue;
    }
    const content = fs.readFileSync(absolute, "utf8");
    for (const line of content.split(/\r?\n/)) {
      if (ABSOLUTE_PATH_RE.test(line)) {
        leaks.push(`${file}: ${line.trim().slice(0, 160)}`);
      }
    }
  }
  return leaks;
}

function expectFile(prefix, relative, what) {
  const absolute = path.join(prefix, relative);
  if (!fs.existsSync(absolute)) {
    fail(`staged SDK is missing ${what} (${relative})`);
    return false;
  }
  return true;
}

function main() {
  const keepFlag = KEEP;
  const prefix = fs.mkdtempSync(path.join(os.tmpdir(), "vc-sdk-prefix-"));
  const consumerDir = fs.mkdtempSync(path.join(os.tmpdir(), "vc-sdk-consumer-"));
  const engineRootLower = ENGINE_ROOT.replace(/\\/g, "/").toLowerCase();

  try {
    log(`engine root: ${ENGINE_ROOT}`);
    log(`install prefix: ${prefix}`);

    // 0. Reconfigure the main build so the new install rules (vc_sdk archive,
    // dependency libs, package config) are in the install manifest.
    log("reconfiguring main build (picks up install rules)");
    let result = run("cmake", ["-S", ".", "-B", "build"], { cwd: ENGINE_ROOT });
    if (result.status !== 0) {
      fail(`main reconfigure exited ${result.status}`);
      return;
    }

    // 1. Install into the fresh prefix.
    log("cmake --install into prefix");
    result = run("cmake", ["--install", BUILD_DIR, "--config", "Release",
      "--prefix", prefix]);
    if (result.status !== 0) {
      fail(`cmake --install exited ${result.status}`);
      return;
    }

    // 2. Verify the staged tree.
    log("verifying staged SDK layout");
    let ok = true;
    ok = expectFile(prefix, "SDK.md", "SDK.md") && ok;
    ok = expectFile(prefix, "include/engine/version.hpp", "public headers") && ok;
    ok = expectFile(prefix, "include/glm/glm.hpp", "glm headers") && ok;
    ok = expectFile(prefix, "lib/vc_sdk.lib", "vc_sdk archive") && ok;
    ok = expectFile(prefix, "lib/Jolt.lib", "Jolt physics lib") && ok;
    ok = expectFile(prefix, "lib/bullet.lib", "Bullet physics lib") && ok;
    for (const lib of ["vc_zstd.lib", "vc_blake3.lib", "vc_navigation.lib",
      "vc_fastwfc.lib", "vc_meshoptimizer.lib", "vc_xatlas.lib",
      "flatbuffers.lib", "rocksdb.lib"]) {
      ok = expectFile(prefix, `lib/${lib}`, lib) && ok;
    }
    ok = expectFile(prefix, "lib/cmake/vulkan_craft_sdk/vulkan_craft_sdk-config.cmake",
      "relocatable package config") && ok;
    ok = expectFile(prefix, "tools/mcp-server/game-authoring.mjs", "MCP server") && ok;
    const publicHeaders = collectTextFiles(path.join(prefix, "include", "engine"))
      .filter((name) => name.endsWith(".hpp"));
    if (publicHeaders.length === 0) {
      fail("no public headers staged under include/engine");
      ok = false;
    } else {
      log(`${publicHeaders.length} public headers staged`);
    }
    if (!ok) return;

    // 3. Staged text files must not reference the engine tree (or any absolute
    // path — the config and MCP must be relocatable).
    const stagedText = collectTextFiles(prefix);
    const engineLeaks = scanForPath(prefix, stagedText, engineRootLower);
    const absoluteLeaks = scanForAbsolutePaths(prefix, stagedText);
    if (engineLeaks.length > 0 || absoluteLeaks.length > 0) {
      for (const leak of [...engineLeaks, ...absoluteLeaks]) {
        fail(`staged tree references the engine tree: ${leak}`);
      }
      return;
    }
    log(`staged ${stagedText.length} text files: no engine-tree or absolute paths`);

    // 4. Consumer matrix (FALTANTES item 11 / §24): the representative
    // (registries + gameplay), the empty minimum (boot + one edit) and an
    // advanced composition (multi-world + portal + gameplay). Each is copied
    // from its template and built against ONLY the prefix (find_package
    // CONFIG; no engine-tree reference anywhere).
    const consumers = [
      { template: "external-project",
        exe: "consumer.exe",
        markers: ["consumer-ok registries", "consumer-ok gameplay", "consumer-ok all"],
        label: "representative (registries + gameplay)" },
      { template: "external-project-empty",
        exe: "empty_consumer.exe",
        markers: ["empty-consumer-ok boot", "empty-consumer-ok edit", "empty-consumer-ok all"],
        label: "empty (minimum boot + edit)" },
      { template: "external-project-advanced",
        exe: "advanced_consumer.exe",
        markers: ["advanced-consumer-ok worlds", "advanced-consumer-ok portal",
          "advanced-consumer-ok gameplay", "advanced-consumer-ok all"],
        label: "advanced (worlds + portal + gameplay)" },
      // AGENT-5 (2026-08-26, findings #159): the META S32 content-pipeline
      // differentials (TimelineGraph/CausalResolver/MacroMicroReconciler/
      // WorldDirector/EpisodeCompiler/HeadlessSimulationFarm/SemanticEngineAPI)
      // claim "self-contained std" but no consumer compiled them against the
      // INSTALLED SDK. This consumer compiles + runs all seven through their
      // public contracts only.
      { template: "external-project-differentials",
        exe: "differentials_consumer.exe",
        markers: ["differentials-consumer-ok timeline", "differentials-consumer-ok causal",
          "differentials-consumer-ok reconciler", "differentials-consumer-ok director",
          "differentials-consumer-ok semantic", "differentials-consumer-ok episode-farm",
          "differentials-consumer-ok all"],
        label: "differentials (S32 content pipeline)" }
    ];
    const allConsumerBuilds = [];
    for (const spec of consumers) {
      const consumer = path.join(consumerDir, spec.template);
      copyDirectory(path.join(ENGINE_ROOT, "tools", spec.template), consumer);
      log(`consumer source (${spec.label}): ${consumer}`);
      log("configuring consumer against the install prefix");
      result = run("cmake", ["-S", consumer, "-B", path.join(consumer, "build"),
        "-G", "Visual Studio 18 2026",
        `-DCMAKE_PREFIX_PATH=${prefix}`]);
      if (result.status !== 0) {
        fail(`consumer ${spec.template} configure exited ${result.status}`);
        return;
      }
      log("building consumer (Release)");
      result = run("cmake", ["--build", path.join(consumer, "build"),
        "--config", "Release"]);
      if (result.status !== 0) {
        fail(`consumer ${spec.template} build exited ${result.status}`);
        return;
      }

      // 5. Run the consumer.
      const consumerExe = path.join(consumer, "build", "Release", spec.exe);
      if (!fs.existsSync(consumerExe)) {
        fail(`${spec.exe} was not produced (${consumerExe})`);
        return;
      }
      log(`running ${spec.exe} (${spec.label})`);
      result = run(consumerExe, []);
      if (result.status !== 0) {
        fail(`${spec.exe} exited ${result.status}`);
        return;
      }
      const output = `${result.stdout || ""}${result.stderr || ""}`;
      for (const marker of spec.markers) {
        if (!output.includes(marker)) {
          fail(`${spec.exe} output missing marker "${marker}"`);
          return;
        }
      }
      log(`${spec.exe} output verified (${spec.markers.join(", ")})`);
      allConsumerBuilds.push(path.join(consumer, "build"));
    }

    // 6. The consumers' build trees must not reference the engine tree: their
    // only dependency is the temp install prefix.
    for (const build of allConsumerBuilds) {
      const consumerText = collectTextFiles(build);
      const consumerLeaks = scanForPath(build, consumerText, engineRootLower);
      if (consumerLeaks.length > 0) {
        for (const leak of consumerLeaks) {
          fail(`consumer build tree references the engine tree: ${leak}`);
        }
        return;
      }
      log(`consumer build tree (${consumerText.length} text files): no engine-tree refs`);
    }

    log("EXTERNAL-CONSUMER GATE PASSED");
    log(`prefix: ${prefix}`);
  } catch (error) {
    fail(`unexpected error: ${error.message}`);
  } finally {
    // Clean up on every path — success, early failure returns, or exception.
    if (!keepFlag) {
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
