#!/usr/bin/env node

// Moved-folder portability gate (task_plan Agente 5 §8 item 2 — "Projeto
// consumidor compila fora da árvore, após instalação, em pasta movida e em
// máquina limpa"). The external-consumer gate proves out-of-tree + after-install
// + clean-machine; this gate closes the missing "pasta movida" clause:
//
//   1. `cmake --install` into a fresh prefix A (from the existing build — no
//      reconfigure, no touching the engine tree).
//   2. MOVE prefix A to a NEW directory B (different path, same machine).
//   3. Scan B's staged text files: the config must contain NO reference to A
//      (the relocated prefix must resolve paths relative to its own location).
//   4. Copy tools/external-project (a consumer whose ONLY dependency is
//      find_package(vulkan_craft_sdk CONFIG)) and build + RUN it against B.
//   5. Verify the consumer build tree has no reference to the engine tree.
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
const BUILD_DIR = path.join(ENGINE_ROOT, process.env.VC_BUILD_DIR ?? "build");
const KEEP = process.argv.includes("--keep");

function log(message) {
  process.stderr.write(`[moved-prefix-gate] ${message}${os.EOL}`);
}

function fail(message) {
  process.stderr.write(`[moved-prefix-gate] FAIL: ${message}${os.EOL}`);
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
      const from = path.join(dir, entry.name);
      const rel = path.join(relative, entry.name);
      if (entry.isDirectory()) walk(from, rel);
      else if (entry.isFile() && /\.(cmake|txt|md|mjs|json|in|hpp)$/i.test(entry.name)) output.push(rel);
    }
  };
  walk(directory, relativePrefix);
  return output;
}

function scanForPath(directory, files, needle) {
  const hits = [];
  for (const rel of files) {
    const source = fs.readFileSync(path.join(directory, rel), "utf8");
    if (source.toLowerCase().includes(needle)) hits.push(rel);
  }
  return hits;
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

function main() {
  const keepFlag = KEEP;
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "vc-moved-prefix-"));
  const prefixA = path.join(work, "prefix-a");
  const prefixB = path.join(work, "prefix-b");
  const consumerDir = path.join(work, "consumer");
  const engineRootLower = ENGINE_ROOT.replace(/\\/g, "/").toLowerCase();

  try {
    log(`engine root: ${ENGINE_ROOT}`);
    log(`prefix A (original): ${prefixA}`);
    log(`prefix B (moved to): ${prefixB}`);
    if (!fs.existsSync(path.join(BUILD_DIR, "CMakeCache.txt"))) {
      fail(`build dir not found: ${BUILD_DIR} (configure the main build first)`);
      return;
    }

    // 1. Install into prefix A (uses the existing install manifest — the
    // install rules are stable, so no reconfigure is needed or performed).
    log("cmake --install into prefix A");
    let result = run("cmake", ["--install", BUILD_DIR, "--config", "Release", "--prefix", prefixA]);
    if (result.status !== 0) {
      fail(`cmake --install exited ${result.status}`);
      return;
    }
    if (!fs.existsSync(path.join(prefixA, "lib/cmake/vulkan_craft_sdk/vulkan_craft_sdk-config.cmake"))) {
      fail("package config was not staged in prefix A");
      return;
    }

    // 2. Move prefix A -> prefix B (fs.renameSync is atomic within the same
    // volume; tmp is usually on the same drive as itself, so a plain rename
    // works. Fall back to copy+remove if the rename crosses volumes.)
    log("moving prefix A -> prefix B");
    try {
      fs.renameSync(prefixA, prefixB);
    } catch {
      copyDirectory(prefixA, prefixB);
      fs.rmSync(prefixA, { recursive: true, force: true });
    }
    if (!fs.existsSync(prefixB)) {
      fail("move failed: prefix B missing");
      return;
    }

    // 3. The moved staged tree must NOT reference the ORIGINAL prefix A: the
    // relocated config resolves paths relative to its own location. This is the
    // heart of the "pasta movida" claim.
    const prefixALower = prefixA.replace(/\\/g, "/").toLowerCase();
    const stagedText = collectTextFiles(prefixB);
    const aLeaks = scanForPath(prefixB, stagedText, prefixALower);
    const engineLeaks = scanForPath(prefixB, stagedText, engineRootLower);
    if (aLeaks.length > 0 || engineLeaks.length > 0) {
      for (const leak of [...aLeaks, ...engineLeaks]) {
        fail(`moved staged tree still references the original location: ${leak}`);
      }
      return;
    }
    log(`moved staged ${stagedText.length} text files: no reference to prefix A or engine tree`);

    // 4. Scaffold a consumer from the INSTALLED project template (task_plan §1
    // "templates" clause): the SDK package ships tools/external-project* under
    // templates/. Copying the scaffold from the moved prefix B proves a user
    // can create a project from the INSTALLED SDK alone (no engine tree), then
    // build + run it against B.
    const installedTemplate = path.join(prefixB, "templates", "external-project");
    if (!fs.existsSync(installedTemplate)) {
      fail(`installed template not staged: ${installedTemplate}`);
      return;
    }
    copyDirectory(installedTemplate, consumerDir);
    log("configuring consumer against the moved prefix B");
    result = run("cmake", ["-S", consumerDir, "-B", path.join(consumerDir, "build"),
      "-G", "Visual Studio 18 2026",
      `-DCMAKE_PREFIX_PATH=${prefixB}`]);
    if (result.status !== 0) {
      fail(`consumer configure against moved prefix exited ${result.status}`);
      return;
    }
    log("building consumer (Release) against the moved prefix");
    result = run("cmake", ["--build", path.join(consumerDir, "build"), "--config", "Release"]);
    if (result.status !== 0) {
      fail(`consumer build against moved prefix exited ${result.status}`);
      return;
    }
    const consumerExe = path.join(consumerDir, "build", "Release", "consumer.exe");
    if (!fs.existsSync(consumerExe)) {
      fail("consumer.exe was not produced");
      return;
    }
    log("running consumer (linked against the moved prefix)");
    result = run(consumerExe, []);
    if (result.status !== 0) {
      fail(`consumer.exe exited ${result.status}`);
      return;
    }
    const output = `${result.stdout || ""}${result.stderr || ""}`;
    for (const marker of ["consumer-ok registries", "consumer-ok gameplay", "consumer-ok all"]) {
      if (!output.includes(marker)) {
        fail(`consumer output missing marker "${marker}"`);
        return;
      }
    }
    log("consumer output verified (registries + gameplay + all)");

    // 5. The consumer build tree must not reference the engine tree.
    const consumerText = collectTextFiles(path.join(consumerDir, "build"));
    const consumerLeaks = scanForPath(path.join(consumerDir, "build"), consumerText, engineRootLower);
    if (consumerLeaks.length > 0) {
      for (const leak of consumerLeaks) fail(`consumer build tree references the engine tree: ${leak}`);
      return;
    }
    log(`consumer build tree (${consumerText.length} text files): no engine-tree refs`);

    log("MOVED-PREFIX GATE PASSED (install -> move -> consumer builds+runs against relocated prefix)");
  } catch (error) {
    fail(`unexpected error: ${error.message}`);
  } finally {
    if (!keepFlag) {
      fs.rmSync(work, { recursive: true, force: true });
      log("temp dirs cleaned");
    }
  }
}

main();
