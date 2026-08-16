#!/usr/bin/env node

// Clean-machine portability gate (FALTANTES item 11 / §24).
//
// Simulates "copiar a engine para outro computador" on this single machine:
//   1. Copies the MINIMAL portable engine tree (src, third_party, tools, tests,
//      the 11 external solutions the build consumes, CMakeLists.txt, SDK.md)
//      to a fresh temporary directory.
//   2. Scans the copy for absolute workspace paths (Windows drive paths, UNC
//      shares, the original engine root) — the §24 "remover paths absolutos"
//      check. (This comment deliberately avoids literal drive-path examples.)
//   3. Configures + builds a small target (voxel_scheduler_tests) in the copy
//      and RUNS it — "criar projeto, compilar, executar na máquina limpa".
//   4. Runs the MCP semantic layer from the copy (create/validate project,
//      author registry assets) — "testar o MCP fora do repositório original".
//   5. Runs `cmake --install` on the copy and verifies the staged SDK is
//      self-contained (include/bin/tools only, no src/build, no absolute
//      paths) and that the MCP works from the installed copy.
//
// Usage:
//   node tools/portability/clean-machine-gate.mjs                  # full gate
//   node tools/portability/clean-machine-gate.mjs --no-build       # skip cmake
//   node tools/portability/clean-machine-gate.mjs --seed-deps DIR  # reuse a
//                        # workspace build/_deps as a local mirror (fast)
//   node tools/portability/clean-machine-gate.mjs --keep           # keep temp
//
// Without --seed-deps the relocated configure fetches glfw/glm/vk-bootstrap/
// vma/miniaudio/imgui over the network (slow). Dependency fetching is a
// build-ENVIRONMENT concern (like installing the Vulkan SDK), not a source
// portability one; --seed-deps isolates the §24 claim (relocatable source, no
// absolute paths, configure/build/run from an arbitrary directory).
//
// Exit code 0 = all checks passed. Build/configure output is streamed to stderr.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { spawnSync } from "node:child_process";

const GATE_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(GATE_DIR, "..", "..");

// The exact external/solutions subdirectories the build consumes (grep
// VC_SOLUTIONS_DIR in CMakeLists.txt). The full catalog is 11 GB; the portable
// engine includes only what it builds with ("somente o necessário", §24).
const CONSUMED_SOLUTIONS = [
  "zstd", "blake3", "flatbuffers", "rocksdb",
  "entt", "recast-navigation", "fast-wfc",
  "delaunator-cpp", "earcut-hpp", "meshoptimizer", "xatlas"
];

// Windows absolute paths: a drive letter (single letter + colon + slash) at a
// word boundary, or a UNC share (leading double backslash). URLs never match
// (the letter before ':' is not at a boundary), and comment lines starting
// with // never match (they use forward slashes).
const ABSOLUTE_PATH_RE = /((^|[^A-Za-z])[A-Za-z]:[\\/])|(^\\\\[^\\/])/;

function log(message) {
  process.stderr.write(`[clean-machine-gate] ${message}${os.EOL}`);
}

function fail(message) {
  process.stderr.write(`[clean-machine-gate] FAIL: ${message}${os.EOL}`);
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

function copyDirectory(source, target, filter) {
  fs.mkdirSync(target, { recursive: true });
  for (const entry of fs.readdirSync(source, { withFileTypes: true })) {
    if (filter && !filter(entry.name)) continue;
    const from = path.join(source, entry.name);
    const to = path.join(target, entry.name);
    if (entry.isDirectory()) copyDirectory(from, to, filter);
    else if (entry.isFile()) fs.copyFileSync(from, to);
  }
}

function treeBytes(directory) {
  let total = 0;
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const absolute = path.join(directory, entry.name);
    if (entry.isDirectory()) total += treeBytes(absolute);
    else if (entry.isFile()) total += fs.statSync(absolute).size;
  }
  return total;
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

function collectTextFiles(directory, relativePrefix = "") {
  const output = [];
  const walk = (dir, relative) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const childRelative = relative ? path.join(relative, entry.name) : entry.name;
      if (entry.isDirectory()) walk(path.join(dir, entry.name), childRelative);
      else if (/\.(cpp|hpp|h|c|cc|mjs|json|cmake|txt|md)$/i.test(entry.name)) output.push(childRelative);
    }
  };
  walk(directory, relativePrefix);
  return output;
}

const argv = process.argv.slice(2);
const args = new Set(argv);
const skipBuild = args.has("--no-build");
const keep = args.has("--keep");
const seedIndex = argv.indexOf("--seed-deps");
const seedDeps = seedIndex >= 0 && argv[seedIndex + 1] ? path.resolve(argv[seedIndex + 1]) : null;

const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), "vc-clean-machine-"));
const copyRoot = path.join(tempRoot, "engine");
const copyBuild = path.join(copyRoot, "build");
const copyDist = path.join(copyRoot, "dist");

(async () => {
  try {
    log(`engine root: ${ENGINE_ROOT}`);
    log(`clean copy: ${copyRoot}`);

    // ── 1. Copy the minimal portable tree ─────────────────────────────────
    fs.mkdirSync(copyRoot, { recursive: true });
    copyDirectory(path.join(ENGINE_ROOT, "src"), path.join(copyRoot, "src"));
    copyDirectory(path.join(ENGINE_ROOT, "third_party"), path.join(copyRoot, "third_party"));
    copyDirectory(path.join(ENGINE_ROOT, "tools"), path.join(copyRoot, "tools"));
    copyDirectory(path.join(ENGINE_ROOT, "tests"), path.join(copyRoot, "tests"));
    copyDirectory(path.join(ENGINE_ROOT, "cmake"), path.join(copyRoot, "cmake"));
    copyDirectory(path.join(ENGINE_ROOT, "shaders"), path.join(copyRoot, "shaders"));
    fs.mkdirSync(path.join(copyRoot, "external", "solutions"), { recursive: true });
    for (const solution of CONSUMED_SOLUTIONS) {
      copyDirectory(
        path.join(ENGINE_ROOT, "external", "solutions", solution),
        path.join(copyRoot, "external", "solutions", solution));
    }
    for (const file of ["README.md", "SNAPSHOT.md"]) {
      const source = path.join(ENGINE_ROOT, "external", "solutions", file);
      if (fs.existsSync(source)) fs.copyFileSync(source, path.join(copyRoot, "external", "solutions", file));
    }
    copyDirectory(path.join(ENGINE_ROOT, "assets"), path.join(copyRoot, "assets"));
    // Exported registry JSON Schemas (FALTANTES item 10): consumed by the
    // editor/CLI/scripting from the portable tree.
    if (fs.existsSync(path.join(ENGINE_ROOT, "schema"))) {
      copyDirectory(path.join(ENGINE_ROOT, "schema"), path.join(copyRoot, "schema"));
    }
    for (const file of ["CMakeLists.txt", "SDK.md"]) {
      fs.copyFileSync(path.join(ENGINE_ROOT, file), path.join(copyRoot, file));
    }
    log(`copied minimal tree (${CONSUMED_SOLUTIONS.length} solutions, ` +
        `${(treeBytes(copyRoot) / 1024 / 1024).toFixed(1)} MB)`);

    // ── 2. No absolute workspace paths in the copy ────────────────────────
    const textFiles = collectTextFiles(path.join(copyRoot, "src"), "src")
      .concat(collectTextFiles(path.join(copyRoot, "tools"), "tools"))
      .concat(collectTextFiles(path.join(copyRoot, "tests"), "tests"))
      .concat(["CMakeLists.txt", "SDK.md"]);
    const leaks = scanForAbsolutePaths(copyRoot, textFiles);
    if (leaks.length) {
      log(`absolute-path leaks found (${leaks.length}):`);
      for (const leak of leaks.slice(0, 20)) log(`  ${leak}`);
      return fail("copied tree contains absolute workspace paths");
    }
    log(`no absolute workspace paths in ${textFiles.length} source files`);

    // ── 3. Configure + build + run a small target in the copy ─────────────
    // The relocated configure disables the optional storage deps
    // (rocksdb/flatbuffers/zstd/blake3): the gate's targets do not link them,
    // RocksDB's configure hangs on Windows, and the MAIN workspace build
    // covers the full set. The gate proves the minimal core (scheduler +
    // physics backends + project/package tools) builds from an arbitrary dir.
    const configureFlags = [
      "-S", copyRoot, "-B", copyBuild,
      "-DVC_ENABLE_ROCKSDB=OFF", "-DVC_ENABLE_FLATBUFFERS=OFF",
      "-DVC_ENABLE_ZSTD=OFF", "-DVC_ENABLE_BLAKE3=OFF"
    ];
    if (!skipBuild) {
      if (seedDeps) {
        fs.mkdirSync(path.join(copyBuild, "_deps"), { recursive: true });
        const seeds = fs.readdirSync(seedDeps).filter((name) => name.endsWith("-src"));
        for (const name of seeds) {
          const from = path.join(seedDeps, name);
          const to = path.join(copyBuild, "_deps", name);
          if (fs.existsSync(from) && !fs.existsSync(to)) {
            log(`seeding FetchContent dep ${name} from the workspace mirror...`);
            fs.cpSync(from, to, { recursive: true });
          }
        }
        log(`seeded ${seeds.length} FetchContent source dirs from ${seedDeps}`);
        // The mirror already has the right revisions; tell FetchContent to
        // treat the build tree as fully populated so it never hits the network.
        configureFlags.push("-DFETCHCONTENT_FULLY_DISCONNECTED=ON");
      } else {
        log("configuring the clean copy (FetchContent will clone glfw/glm/vk-bootstrap/vma/miniaudio/imgui over the network; this can be slow)...");
      }
      log("configuring the clean copy...");
      let result = run("cmake", configureFlags);
      if (result.status !== 0) return fail("cmake configure failed in the clean copy");
      log("building voxel_scheduler_tests + tools + the four app exes in the clean copy...");
      result = run("cmake", ["--build", copyBuild, "--config", "Release",
        "--target", "voxel_scheduler_tests", "VulkanProjectGenerator", "VulkanPackageBuilder",
        "VulkanEngineGame", "VulkanEngineEditor", "VulkanEngineServer", "VulkanEngineCooker"]);
      if (result.status !== 0) return fail("cmake build failed in the clean copy");
      const testExe = path.join(copyBuild, "Release", "voxel_scheduler_tests.exe");
      if (!fs.existsSync(testExe)) return fail(`test executable missing: ${testExe}`);
      log("running voxel_scheduler_tests from the clean copy...");
      result = run(testExe, []);
      if (result.status !== 0) return fail("relocated test binary failed");
      if (!fs.existsSync(path.join(copyBuild, "Release", "VulkanProjectGenerator.exe"))) {
        return fail("relocated VulkanProjectGenerator.exe missing");
      }
      // Headless app smoke-runs from the relocated build (Fase 9 pendente):
      // the dedicated server runs a bounded headless sim; the cooker prints
      // usage without args (proves the exe loads and runs from the copy).
      // VulkanEngineGame/Editor need a GPU window — build-only here (same
      // user-decision caveat as visual validation).
      const serverExe = path.join(copyBuild, "Release", "VulkanEngineServer.exe");
      if (!fs.existsSync(serverExe)) return fail("relocated VulkanEngineServer.exe missing");
      result = run(serverExe, ["--ticks", "10"]);
      if (result.status !== 0 || !/completed 10 headless ticks/.test(result.stdout || "")) {
        return fail("relocated VulkanEngineServer headless run failed");
      }
      log("relocated VulkanEngineServer --ticks 10: PASS (headless dedicated server)");
      const cookerExe = path.join(copyBuild, "Release", "VulkanEngineCooker.exe");
      if (!fs.existsSync(cookerExe)) return fail("relocated VulkanEngineCooker.exe missing");
      const cooker = run(cookerExe, []);
      const cookerOut = (cooker.stdout || "") + (cooker.stderr || "");
      if (cooker.status === 0 || !/^Usage: VulkanEngineCooker/.test(cookerOut)) {
        return fail("relocated VulkanEngineCooker did not print usage (unexpected)");
      }
      log("relocated VulkanEngineCooker: PASS (usage printed, exe runs from the copy)");
      for (const name of ["VulkanEngineGame.exe", "VulkanEngineEditor.exe"]) {
        if (!fs.existsSync(path.join(copyBuild, "Release", name))) {
          return fail(`relocated ${name} missing (build-only, needs GPU)`);
        }
      }
      log("relocated VulkanEngineGame/Editor built (GPU window required to run — build-only)");
      log("relocated build + run: PASS");
    } else {
      log("skipping configure/build/install (--no-build)");
    }

    // ── 4. MCP from the copy (outside the original repository) ────────────
    const { callSemanticTool } = await import(
      pathToFileURL(path.join(copyRoot, "tools", "mcp-server", "game-authoring.mjs")).href);
    const projectName = `CleanMachine${Date.now()}`;
    callSemanticTool(copyRoot, "create_game_project", { name: projectName, starter_scene: false });
    const manifest = JSON.parse(
      fs.readFileSync(path.join(copyRoot, "Projects", projectName, "project.json"), "utf8"));
    if (manifest.engine !== "../..") return fail(`project engine ref not relative: ${manifest.engine}`);
    const block = callSemanticTool(copyRoot, "author_registry_asset", {
      project: projectName, kind: "block", name: "Titanium", hardness: 3.5, tags: ["metal"] });
    if (!block.created || block.diagnostics.length) return fail("MCP block authoring failed in the copy");
    const recipe = callSemanticTool(copyRoot, "author_registry_asset", {
      project: projectName, kind: "recipe", name: "TitaniumIngot",
      inputs: [{ item: "vulkancraft:titanium", count: 2 }],
      outputs: [{ item: "vulkancraft:titanium_ingot", count: 1 }] });
    if (!recipe.created) return fail("MCP recipe authoring failed in the copy");
    const validation = callSemanticTool(copyRoot, "validate_game_project", { project: projectName });
    if (!validation.valid) return fail(`MCP validation failed in the copy: ${JSON.stringify(validation)}`);
    log(`MCP from the copy: project '${projectName}' created, registry assets authored, validation OK`);

    // ── 5. Staged SDK install is self-contained ───────────────────────────
    if (!skipBuild) {
      log("installing the redistributable SDK from the clean copy...");
      let result = run("cmake", ["--install", copyBuild, "--prefix", copyDist]);
      if (result.status !== 0) return fail("cmake --install failed in the clean copy");
      const distEntries = fs.readdirSync(copyDist);
      const forbidden = distEntries.filter((name) =>
        ["src", "build", "tests", "external"].includes(name) || name === "CMakeLists.txt");
      if (forbidden.length) return fail(`staged SDK contains build-only entries: ${forbidden.join(", ")}`);
      if (!fs.existsSync(path.join(copyDist, "include", "engine"))) return fail("staged SDK has no include/engine");
      if (!fs.existsSync(path.join(copyDist, "tools", "mcp-server", "server.mjs"))) return fail("staged SDK has no MCP server");
      if (!fs.existsSync(path.join(copyDist, "bin", "VulkanProjectGenerator.exe"))) return fail("staged SDK has no built tool in bin");
      const stagedFiles = collectTextFiles(path.join(copyDist, "include"), "include")
        .concat(collectTextFiles(path.join(copyDist, "tools"), "tools"))
        .concat(collectTextFiles(path.join(copyDist, "bin"), "bin"))
        .concat(["SDK.md"]);
      const stagedLeaks = scanForAbsolutePaths(copyDist, stagedFiles);
      if (stagedLeaks.length) {
        for (const leak of stagedLeaks.slice(0, 10)) log(`  ${leak}`);
        return fail("staged SDK contains absolute workspace paths");
      }
      const { callSemanticTool: stagedCall } = await import(
        pathToFileURL(path.join(copyDist, "tools", "mcp-server", "game-authoring.mjs")).href);
      const capabilities = stagedCall(copyDist, "game_capabilities", {});
      if (!capabilities.registry_asset_kinds) return fail("staged MCP capabilities missing registry_asset_kinds");
      log(`staged SDK self-contained: include/engine + tools/mcp-server + bin, ` +
          `${stagedFiles.length} files scanned, MCP loads from the install`);
    }

    log("CLEAN-MACHINE GATE PASSED");
  } catch (error) {
    fail(error.stack || String(error));
  } finally {
    if (!keep) {
      fs.rmSync(tempRoot, { recursive: true, force: true });
      log("temp copy removed");
    } else {
      log(`temp copy kept at ${copyRoot}`);
    }
  }
})();
