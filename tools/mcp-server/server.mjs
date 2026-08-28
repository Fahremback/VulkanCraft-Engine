#!/usr/bin/env node

import crypto from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { callSemanticTool, semanticToolDefinitions, listProjects, inspectProject } from "./game-authoring.mjs";
import { callControlApiTool, controlApiToolDefinitions } from "./control-api.mjs";
import { callPublicRuntimeTool, publicRuntimeTools } from "./contract-runtime.mjs";

const SERVER_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(SERVER_DIR, "..", "..");
const SERVER_NAME = "vulkancraft-engine";
const SERVER_VERSION = "1.2.0";
// The single MCP protocol version this server implements. Clients request a
// version in `initialize`; the server MUST refuse any version it does not
// implement (echoing an unsupported version would make the client believe it
// can use features this server does not have).
// Both published MCP spec versions are accepted; the latest is the default
// when the client omits protocolVersion. The negotiated version is echoed
// back in the initialize result (findings #234-mcp-multiversion).
const SUPPORTED_PROTOCOL_VERSIONS = ["2024-11-05", "2025-03-26"];
const SUPPORTED_PROTOCOL_VERSION = SUPPORTED_PROTOCOL_VERSIONS[SUPPORTED_PROTOCOL_VERSIONS.length - 1];

// Engine semantic version, parsed from the SINGLE source of truth
// (src/engine/public/engine/version.hpp — engine_version()), so the MCP
// surface can never drift from the C++ API. Tolerant parse: if the header
// shape changes, fall back to a documented sentinel instead of guessing.
function parseEngineVersion() {
  try {
    const src = fs.readFileSync(path.join(ENGINE_ROOT, "src/engine/public/engine/version.hpp"), "utf8");
    const match = src.match(/return\s+VersionInfo\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}/);
    if (match) {
      return {
        major: Number(match[1]),
        minor: Number(match[2]),
        patch: Number(match[3]),
        codename: match[4],
        abi: match[5],
        string: `${match[1]}.${match[2]}.${match[3]}`
      };
    }
  } catch {
    // fall through to sentinel
  }
  return { major: 0, minor: 0, patch: 0, codename: "unknown", abi: "unknown", string: "0.0.0" };
}
const ENGINE_VERSION = parseEngineVersion();
const MAX_READ_LINES = 500;
const MAX_TEXT_BYTES = 2 * 1024 * 1024;
const DEFAULT_SEARCH_RESULTS = 80;

const BLOCKED_WRITE_ROOTS = new Set([
  "build",
  "build-no-voxel",
  "build-novoxel",
  "intermediate",
  ".git"
]);

const TEXT_EXTENSIONS = new Set([
  ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
  ".glsl", ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
  ".md", ".txt", ".json", ".toml", ".yaml", ".yml", ".cmake",
  ".ini", ".cfg", ".script", ".scene", ".material", ".prefab"
]);

function log(message) {
  process.stderr.write(`[${SERVER_NAME}] ${message}${os.EOL}`);
}

function sha256(text) {
  return crypto.createHash("sha256").update(text, "utf8").digest("hex");
}

function normalizeRelative(input = ".") {
  if (typeof input !== "string" || input.includes("\0")) {
    throw new Error("path must be a valid string");
  }
  const absolute = path.resolve(ENGINE_ROOT, input.replaceAll("/", path.sep));
  const relative = path.relative(ENGINE_ROOT, absolute);
  if (relative.startsWith("..") || path.isAbsolute(relative)) {
    throw new Error("path escapes the engine root");
  }
  return { absolute, relative: relative || "." };
}

function assertWritable(relative) {
  const first = relative.split(path.sep)[0].toLowerCase();
  if (BLOCKED_WRITE_ROOTS.has(first)) {
    throw new Error(`writes under '${first}' are blocked`);
  }
  if (relative === ".") throw new Error("the engine root cannot be overwritten");
}

function isTextFile(filePath) {
  const base = path.basename(filePath).toLowerCase();
  return base === "cmakelists.txt" || TEXT_EXTENSIONS.has(path.extname(base));
}

function readText(relativePath) {
  const resolved = normalizeRelative(relativePath);
  const stat = fs.statSync(resolved.absolute);
  if (!stat.isFile()) throw new Error("path is not a file");
  if (stat.size > MAX_TEXT_BYTES) throw new Error(`file exceeds ${MAX_TEXT_BYTES} bytes`);
  if (!isTextFile(resolved.absolute)) throw new Error("only known text files can be read");
  const content = fs.readFileSync(resolved.absolute, "utf8");
  if (content.includes("\0")) throw new Error("binary content is not supported");
  return { ...resolved, content, stat };
}

function atomicWrite(absolute, content) {
  fs.mkdirSync(path.dirname(absolute), { recursive: true });
  const temp = path.join(
    path.dirname(absolute),
    `.${path.basename(absolute)}.${process.pid}.${crypto.randomUUID()}.tmp`
  );
  fs.writeFileSync(temp, content, "utf8");
  try {
    fs.renameSync(temp, absolute);
  } catch (error) {
    try { fs.rmSync(temp, { force: true }); } catch {}
    throw error;
  }
}

function jsonText(value) {
  return [{ type: "text", text: JSON.stringify(value, null, 2) }];
}

function errorResult(error) {
  return {
    isError: true,
    content: [{ type: "text", text: error instanceof Error ? error.message : String(error) }]
  };
}

function walkDirectory(relativePath, depth, maxEntries) {
  const root = normalizeRelative(relativePath);
  if (!fs.statSync(root.absolute).isDirectory()) throw new Error("path is not a directory");
  const output = [];

  function visit(current, currentRelative, remainingDepth) {
    if (output.length >= maxEntries) return;
    const entries = fs.readdirSync(current, { withFileTypes: true })
      .filter((entry) => !entry.name.startsWith("."))
      .sort((a, b) => Number(b.isDirectory()) - Number(a.isDirectory()) || a.name.localeCompare(b.name));
    for (const entry of entries) {
      if (output.length >= maxEntries) break;
      const rel = path.join(currentRelative === "." ? "" : currentRelative, entry.name);
      output.push({ path: rel.replaceAll(path.sep, "/"), type: entry.isDirectory() ? "directory" : "file" });
      if (entry.isDirectory() && remainingDepth > 0 && !BLOCKED_WRITE_ROOTS.has(entry.name.toLowerCase())) {
        visit(path.join(current, entry.name), rel, remainingDepth - 1);
      }
    }
  }

  visit(root.absolute, root.relative, depth);
  return { root: root.relative.replaceAll(path.sep, "/"), entries: output, truncated: output.length >= maxEntries };
}

function fallbackSearch(query, relativePath, maxResults, useRegex, caseSensitive) {
  const root = normalizeRelative(relativePath);
  const pattern = useRegex ? new RegExp(query, caseSensitive ? "" : "i") : null;
  const needle = caseSensitive ? query : query.toLowerCase();
  const results = [];

  function visit(current) {
    if (results.length >= maxResults) return;
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      if (results.length >= maxResults) break;
      if (entry.name.startsWith(".")) continue;
      const absolute = path.join(current, entry.name);
      if (entry.isDirectory()) {
        if (!BLOCKED_WRITE_ROOTS.has(entry.name.toLowerCase())) visit(absolute);
      } else if (isTextFile(absolute) && fs.statSync(absolute).size <= MAX_TEXT_BYTES) {
        const lines = fs.readFileSync(absolute, "utf8").split(/\r?\n/);
        for (let index = 0; index < lines.length && results.length < maxResults; index++) {
          const haystack = caseSensitive ? lines[index] : lines[index].toLowerCase();
          if ((pattern && pattern.test(lines[index])) || (!pattern && haystack.includes(needle))) {
            results.push({
              path: path.relative(ENGINE_ROOT, absolute).replaceAll(path.sep, "/"),
              line: index + 1,
              text: lines[index].trim().slice(0, 500)
            });
          }
        }
      }
    }
  }

  if (fs.statSync(root.absolute).isDirectory()) visit(root.absolute);
  else throw new Error("search path must be a directory");
  return results;
}

function searchCode(args) {
  const query = String(args.query ?? "");
  if (!query) throw new Error("query is required");
  const relativePath = String(args.path ?? "src");
  const root = normalizeRelative(relativePath);
  const maxResults = Math.min(Math.max(Number(args.max_results ?? DEFAULT_SEARCH_RESULTS), 1), 300);
  const useRegex = Boolean(args.regex);
  const caseSensitive = Boolean(args.case_sensitive);
  const rgArgs = ["--line-number", "--no-heading", "--color", "never", "--max-count", String(maxResults)];
  if (!useRegex) rgArgs.push("--fixed-strings");
  if (!caseSensitive) rgArgs.push("--ignore-case");
  if (typeof args.glob === "string" && args.glob) rgArgs.push("--glob", args.glob);
  rgArgs.push(query, root.absolute);
  // Bound the scan: a pathological tree (no .gitignore in non-git workspaces)
  // can make rg take seconds; timeout falls back to the in-JS search instead
  // of hanging the server forever.
  const result = spawnSync("rg", rgArgs, { encoding: "utf8", windowsHide: true, maxBuffer: 4 * 1024 * 1024, timeout: 15000 });
  if (!result.error && (result.status === 0 || result.status === 1)) {
    const matches = result.stdout.split(/\r?\n/).filter(Boolean).slice(0, maxResults).map((line) => {
      const match = line.match(/^(.*?):(\d+):(.*)$/);
      if (!match) return { text: line.slice(0, 500) };
      return {
        path: path.relative(ENGINE_ROOT, match[1]).replaceAll(path.sep, "/"),
        line: Number(match[2]),
        text: match[3].trim().slice(0, 500)
      };
    });
    return { query, root: relativePath, matches, truncated: matches.length >= maxResults };
  }
  const matches = fallbackSearch(query, relativePath, maxResults, useRegex, caseSensitive);
  return { query, root: relativePath, matches, truncated: matches.length >= maxResults, fallback: true };
}

function engineOverview() {
  const top = ["src/engine", "src/editor", "src/app", "src/tools", "src/plugins", "src/simulation", "tests", "Projects"];
  const modules = top.filter((entry) => fs.existsSync(path.join(ENGINE_ROOT, entry))).map((entry) => {
    const absolute = path.join(ENGINE_ROOT, entry);
    const children = fs.statSync(absolute).isDirectory()
      ? fs.readdirSync(absolute, { withFileTypes: true }).filter((item) => item.isDirectory()).map((item) => item.name).sort()
      : [];
    return { path: entry, children };
  });
  const cmake = fs.readFileSync(path.join(ENGINE_ROOT, "CMakeLists.txt"), "utf8");
  // Direct targets from add_executable/add_library, plus targets generated by
  // foreach loops (e.g. `foreach(vc_exe VulkanEngineGame ... VulkanEngineEditor)`)
  // — the app/editor exes are registered that way and would otherwise be missed.
  const targets = [...new Set([
    ...[...cmake.matchAll(/add_(?:executable|library)\s*\(\s*([^\s\)]+)/g)].map((match) => match[1]),
    ...[...cmake.matchAll(/foreach\(\s*[^\s\)]+\s+([^\n\)]+)/g)].flatMap((match) => match[1].trim().split(/\s+/))
  ])].sort();
  return {
    root: ENGINE_ROOT,
    architecture: "Runtime + Editor + Tools + Plugins + Projects",
    modules,
    cmake_targets: targets,
    authoritative_docs: ["README.md", "docs/ARCHITECTURE.md", "docs/MIGRATION_STATUS.md", "docs/FALTANTES.md"],
    concurrency_rule: "Read before edit and pass expected_sha256 to every edit of an existing file."
  };
}

function versionTool() {
  return {
    engine: ENGINE_VERSION.string,
    engine_abi: ENGINE_VERSION.abi,
    codename: ENGINE_VERSION.codename,
    major: ENGINE_VERSION.major,
    minor: ENGINE_VERSION.minor,
    patch: ENGINE_VERSION.patch,
    server: SERVER_VERSION,
    protocol: SUPPORTED_PROTOCOL_VERSION
  };
}

function pendingStatus() {
  const file = readText("docs/FALTANTES.md");
  const pending = file.content.split(/\r?\n/)
    .map((line, index) => ({ line: index + 1, text: line.trim() }))
    .filter((item) => item.text.startsWith("- [ ]"))
    .map((item) => ({ line: item.line, item: item.text.replace(/^- \[ \]\s*/, "") }));
  return { source: "docs/FALTANTES.md", sha256: sha256(file.content), pending };
}

function inspectSymbol(args) {
  const symbol = String(args.symbol ?? "").trim();
  if (!/^[A-Za-z_~][A-Za-z0-9_:~]*$/.test(symbol)) throw new Error("symbol must be a C/C++ identifier or qualified name");
  const escaped = symbol.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  return searchCode({
    query: `\\b${escaped.split("::").at(-1)}\\b`,
    path: args.path ?? "src",
    regex: true,
    case_sensitive: true,
    max_results: args.max_results ?? 100,
    glob: args.glob ?? "*.{h,hpp,cpp,cxx,cc}"
  });
}

function readFileTool(args) {
  const file = readText(args.path);
  const lines = file.content.split(/\r?\n/);
  const start = Math.min(Math.max(Number(args.start_line ?? 1), 1), Math.max(lines.length, 1));
  const requestedEnd = Number(args.end_line ?? (start + 199));
  const end = Math.min(Math.max(requestedEnd, start), lines.length, start + MAX_READ_LINES - 1);
  const numbered = lines.slice(start - 1, end).map((line, index) => `${start + index}: ${line}`).join("\n");
  return {
    path: file.relative.replaceAll(path.sep, "/"),
    sha256: sha256(file.content),
    total_lines: lines.length,
    start_line: start,
    end_line: end,
    content: numbered
  };
}

function applyTextEdits(args) {
  const file = readText(args.path);
  assertWritable(file.relative);
  const currentHash = sha256(file.content);
  if (typeof args.expected_sha256 !== "string" || args.expected_sha256 !== currentHash) {
    throw new Error(`concurrent modification detected: expected_sha256 must equal ${currentHash}`);
  }
  if (!Array.isArray(args.edits) || args.edits.length === 0) throw new Error("edits must be a non-empty array");
  let updated = file.content;
  const applied = [];
  for (let index = 0; index < args.edits.length; index++) {
    const edit = args.edits[index];
    if (!edit || typeof edit.old_text !== "string" || typeof edit.new_text !== "string") {
      throw new Error(`edit ${index} requires old_text and new_text strings`);
    }
    if (!edit.old_text) throw new Error(`edit ${index} old_text cannot be empty`);
    const occurrences = updated.split(edit.old_text).length - 1;
    if (occurrences === 0) throw new Error(`edit ${index} old_text was not found`);
    if (!edit.replace_all && occurrences !== 1) {
      throw new Error(`edit ${index} matched ${occurrences} times; provide more context or set replace_all`);
    }
    updated = edit.replace_all
      ? updated.split(edit.old_text).join(edit.new_text)
      : updated.replace(edit.old_text, edit.new_text);
    applied.push({ index, replacements: edit.replace_all ? occurrences : 1 });
  }
  if (Buffer.byteLength(updated, "utf8") > MAX_TEXT_BYTES) throw new Error("edited file exceeds size limit");
  const nextHash = sha256(updated);
  if (!args.dry_run) atomicWrite(file.absolute, updated);
  return {
    path: file.relative.replaceAll(path.sep, "/"),
    dry_run: Boolean(args.dry_run),
    before_sha256: currentHash,
    after_sha256: nextHash,
    changed: currentHash !== nextHash,
    applied
  };
}

function createFile(args) {
  const resolved = normalizeRelative(args.path);
  assertWritable(resolved.relative);
  if (!isTextFile(resolved.absolute)) throw new Error("only known text files can be created");
  const content = String(args.content ?? "");
  if (Buffer.byteLength(content, "utf8") > MAX_TEXT_BYTES) throw new Error("content exceeds size limit");
  const exists = fs.existsSync(resolved.absolute);
  if (exists && !args.overwrite) throw new Error("file already exists; use apply_text_edits for existing files");
  if (exists) {
    const current = readText(args.path);
    const currentHash = sha256(current.content);
    if (args.expected_sha256 !== currentHash) {
      throw new Error(`concurrent modification detected: expected_sha256 must equal ${currentHash}`);
    }
  }
  if (!args.dry_run) atomicWrite(resolved.absolute, content);
  return {
    path: resolved.relative.replaceAll(path.sep, "/"),
    dry_run: Boolean(args.dry_run),
    created: !exists,
    sha256: sha256(content),
    bytes: Buffer.byteLength(content, "utf8")
  };
}

// ─── Run / logs: ciclo de validação sem abrir cliente visual ──────────────
// O jogo/editor/server é executado por um tempo limitado com stdout/stderr
// capturados em Projects/.runs/<exe>-<timestamp>.log; as ferramentas de log
// permitem olhar a saída depois, sem manter janela aberta.
const GAME_RUN_ROOT = path.join(ENGINE_ROOT, "Projects", ".runs");
const RUNNABLE_EXES = new Map([
  ["VulkanEngineGame", "build/Release/VulkanEngineGame.exe"],
  ["VulkanEngineEditor", "build/Release/VulkanEngineEditor.exe"],
  ["VulkanEngineServer", "build/Release/VulkanEngineServer.exe"],
  ["VulkanEngineCooker", "build/Release/VulkanEngineCooker.exe"],
  ["vulkan_craft", "build/Release/vulkan_craft.exe"]
]);

function tailLines(filePath, count) {
  const lines = fs.readFileSync(filePath, "utf8").split(/\r?\n/);
  return lines.slice(-Math.max(count, 1)).join("\n");
}

async function runGameTool(args) {
  const exe = String(args.exe ?? "VulkanEngineGame");
  const exeRelative = RUNNABLE_EXES.get(exe);
  if (!exeRelative) throw new Error(`unknown exe '${exe}'; allowed: ${[...RUNNABLE_EXES.keys()].join(", ")}`);
  const exePath = path.join(ENGINE_ROOT, exeRelative);
  if (!fs.existsSync(exePath)) throw new Error(`not built: ${exeRelative} (rode build_game primeiro)`);
  const seconds = Math.min(Math.max(Number(args.seconds ?? 10), 1), 120);
  const extraArgs = Array.isArray(args.args) ? args.args.map(String) : [];

  fs.mkdirSync(GAME_RUN_ROOT, { recursive: true });
  const name = `${exe}-${new Date().toISOString().replace(/[:.]/g, "-")}`;
  const logPath = path.join(GAME_RUN_ROOT, `${name}.log`);
  const stream = fs.createWriteStream(logPath, { flags: "a" });

  const child = spawn(exePath, extraArgs, { cwd: ENGINE_ROOT, windowsHide: true });
  child.stdout.on("data", (chunk) => stream.write(chunk));
  child.stderr.on("data", (chunk) => stream.write(chunk));

  const outcome = await new Promise((resolve) => {
    const timer = setTimeout(() => {
      child.kill();
      resolve({ terminated: "timeout", exit_code: null });
    }, seconds * 1000);
    child.on("exit", (code, signal) => {
      clearTimeout(timer);
      resolve({ terminated: "exit", exit_code: code, signal });
    });
    child.on("error", (error) => {
      clearTimeout(timer);
      resolve({ terminated: "error", error: error.message });
    });
  });

  stream.end();
  const tail = tailLines(logPath, 80);
  return {
    exe,
    log: path.relative(ENGINE_ROOT, logPath).replaceAll(path.sep, "/"),
    log_lines: tail.split("\n").length,
    duration_seconds: seconds,
    ...outcome,
    tail
  };
}

// task_plan Agente 5 §4 item 3 — "cobrir ... empacotar projetos". The engine's
// VulkanPackageBuilder (C++) publishes a distributable package: Bin/<exe> +
// Content/ + PackageManifest.txt, all-or-nothing (staging + rename; removes the
// staging dir on failure). This tool drives it through the same run/log
// pattern as runGameTool (spawn + timeout + log + kill), so "empacotar" is
// reachable from MCP/CLI/editor like every other capability.
function packageGameTool(args) {
  const project = String(args.project ?? "").trim();
  if (!project) throw new Error("project is required");
  const projectRoot = path.join(ENGINE_ROOT, "Projects", project);
  if (!fs.existsSync(path.join(projectRoot, "project.json"))) throw new Error(`project '${project}' does not exist`);
  const exe = String(args.exe ?? "VulkanEngineGame");
  const exeRelative = RUNNABLE_EXES.get(exe);
  if (!exeRelative) throw new Error(`unknown exe '${exe}'; allowed: ${[...RUNNABLE_EXES.keys()].join(", ")}`);
  const exePath = path.join(ENGINE_ROOT, exeRelative);
  if (!fs.existsSync(exePath)) throw new Error(`not built: ${exeRelative} (rode build_game primeiro)`);
  const contentDir = path.join(projectRoot, "Content");
  if (!fs.existsSync(contentDir)) throw new Error(`project has no Content dir: ${contentDir}`);
  const platform = String(args.platform ?? "windows-x64");
  const configuration = String(args.configuration ?? "Shipping");

  fs.mkdirSync(GAME_RUN_ROOT, { recursive: true });
  const name = `package-${project}-${exe}-${new Date().toISOString().replace(/[:.]/g, "-")}`;
  const logPath = path.join(GAME_RUN_ROOT, `${name}.log`);
  const output = path.join(projectRoot, "Package");
  const stream = fs.createWriteStream(logPath, { flags: "a" });
  const child = spawn(path.join(ENGINE_ROOT, "build/Release/VulkanPackageBuilder.exe"),
    [exePath, contentDir, output, platform, configuration],
    { cwd: ENGINE_ROOT, windowsHide: true });
  child.stdout.on("data", (chunk) => stream.write(chunk));
  child.stderr.on("data", (chunk) => stream.write(chunk));

  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      child.kill();
      stream.end();
      resolve({ exe, project, status: "timeout", log: path.relative(ENGINE_ROOT, logPath).replaceAll(path.sep, "/") });
    }, 120000);
    child.on("exit", (code, signal) => {
      clearTimeout(timer);
      stream.end();
      const tail = tailLines(logPath, 40);
      const manifestPath = path.join(output, "PackageManifest.txt");
      const manifest = fs.existsSync(manifestPath) ? readText(manifestPath).content : null;
      resolve({
        exe,
        project,
        status: code === 0 ? "published" : "failed",
        exit_code: code,
        signal: signal ?? null,
        log: path.relative(ENGINE_ROOT, logPath).replaceAll(path.sep, "/"),
        output: `Projects/${project}/Package`,
        manifest,
        tail
      });
    });
    child.on("error", (error) => {
      clearTimeout(timer);
      stream.end();
      reject(error);
    });
  });
}

function listGameLogsTool() {
  if (!fs.existsSync(GAME_RUN_ROOT)) return { logs: [], dir: "Projects/.runs" };
  const logs = fs.readdirSync(GAME_RUN_ROOT)
    .filter((entry) => entry.endsWith(".log"))
    .sort()
    .reverse()
    .map((entry) => {
      const stat = fs.statSync(path.join(GAME_RUN_ROOT, entry));
      return { name: entry, bytes: stat.size, modified: stat.mtime.toISOString() };
    });
  return { logs, dir: "Projects/.runs" };
}

function readGameLogTool(args) {
  const name = String(args.log ?? "");
  if (!/^[A-Za-z0-9._-]+\.log$/.test(name)) {
    throw new Error("log must match [A-Za-z0-9._-]+.log");
  }
  const logPath = path.join(GAME_RUN_ROOT, name);
  if (!fs.existsSync(logPath)) throw new Error(`log not found: ${name} (veja list_game_logs)`);
  const lines = fs.readFileSync(logPath, "utf8").split(/\r?\n/);
  const last = Math.min(Math.max(Number(args.lines ?? 100), 1), 2000);
  return {
    log: name,
    total_lines: lines.length,
    tail: lines.slice(-last).join("\n")
  };
}

function buildGameTool(args) {
  // Synchronous builds remain an explicit legacy operation; long work must use start_build.
  if (args.async === true) return startBuildTool(args);
  const exe = String(args.exe ?? "VulkanEngineGame");
  const known = new Set(["VulkanEngineGame", "VulkanEngineEditor", "VulkanEngineServer",
    "VulkanEngineCooker", "vulkan_craft", "ALL_BUILD"]);
  if (!known.has(exe)) throw new Error(`unknown target '${exe}'`);
  const config = String(args.config ?? "Release");
  const buildRoot = path.join(ENGINE_ROOT, "build");
  if (!fs.existsSync(buildRoot)) throw new Error(`build dir not found: ${buildRoot}`);

  fs.mkdirSync(GAME_RUN_ROOT, { recursive: true });
  const name = `build-${exe}-${new Date().toISOString().replace(/[:.]/g, "-")}.log`;
  const logPath = path.join(GAME_RUN_ROOT, name);
  const argsList = ["--build", "build", "--config", config];
  if (exe !== "ALL_BUILD") argsList.push("--target", exe);
  // Bounded: a hung build must never wedge the MCP server forever.
  const result = spawnSync("cmake", argsList, { cwd: ENGINE_ROOT, encoding: "utf8", windowsHide: true, maxBuffer: 32 * 1024 * 1024, timeout: 15 * 60 * 1000 });
  const output = `${result.stdout ?? ""}${result.stderr ?? ""}`;
  fs.writeFileSync(logPath, output, "utf8");
  const lines = output.split(/\r?\n/);
  const errors = lines.filter((line) => /\berror\b/i.test(line)).slice(0, 20);
  return {
    target: exe,
    log: path.relative(ENGINE_ROOT, logPath).replaceAll(path.sep, "/"),
    status: result.error ? (result.error.code === "ETIMEDOUT" ? "timeout" : "error") : result.status,
    timed_out: result.error?.code === "ETIMEDOUT",
    errors: errors.length,
    error_lines: errors,
    tail: lines.slice(-60).join("\n")
  };
}

// FALTANTES item 5 (MCP server) — subscriptions/events. A client subscribes
// to an event kind (e.g. build.status_changed) and the server emits a
// `notifications/...` message whenever that event fires, interleaved with the
// normal request/response stream. Support is OPT-IN via subscribe_events;
// without a subscription the server emits nothing (no notification spam).
// Events are delivered in the order they fire; a subscriber receives ONLY the
// kinds it subscribed to (topic fan-out by kind).
const EVENT_SUBSCRIPTIONS = new Map();  // eventKind -> Set<subscriptionId>
const EVENT_TOPICS = new Set([
  "build.status_changed",
  "asset.changed",
  "scene.changed",
  "game.status_changed",
  "test.status_changed",
  "profiler.sampled"
]);
let nextSubscriptionId = 1;

// FALTANTES item 5 — optional remote transport. The server is stdio-first
// (the MCP `command`/`args` transport), but can also run over HTTP + SSE with
// `node server.mjs --http [--port N]`. JSON-RPC arrives via POST /mcp and the
// response returns on the same connection; `emitEvent` broadcasts
// `notifications/*` to every open SSE client (GET /events), so a reconnecting
// client keeps receiving events for the kinds it already subscribed to.
// §5 item 7 (auth) — OPTIONAL bearer token for the HTTP transport, off by
// default (MCP is a local protocol; the deliberate default stays auth-free).
// When MCP_AUTH_TOKEN is set, every POST /mcp and GET /events must carry
// `Authorization: Bearer <token>` — stdio is unaffected (local pipe).
const AUTH_TOKEN = process.env.MCP_AUTH_TOKEN || null;

let transportMode = "stdio";  // "stdio" | "http"
const sseClients = new Set();  // Set<http.ServerResponse>
let httpServer = null;
const SERVER_START = Date.now();

function deliverNotification(message) {
  if (transportMode === "http") {
    const frame = `data: ${JSON.stringify(message)}\n\n`;
    for (const res of sseClients) {
      try { res.write(frame); } catch { sseClients.delete(res); }
    }
    return;
  }
  send(message);
}

function emitEvent(kind, payload) {
  const subs = EVENT_SUBSCRIPTIONS.get(kind);
  if (!subs || subs.size === 0) return;
  for (const subId of subs) {
    deliverNotification({
      jsonrpc: "2.0",
      method: `notifications/${kind}`,
      params: { subscription_id: subId, kind, ...payload }
    });
  }
}

function subscribeEventsTool(args) {
  const kinds = Array.isArray(args.kinds) ? args.kinds.map(String) : [];
  const unknown = kinds.filter((k) => !EVENT_TOPICS.has(k));
  if (unknown.length > 0) {
    throw new Error(`unknown event kind(s): ${unknown.join(", ")}; available: ${[...EVENT_TOPICS].join(", ")}`);
  }
  if (kinds.length === 0) {
    throw new Error(`kinds must be a non-empty array; available: ${[...EVENT_TOPICS].join(", ")}`);
  }
  const id = nextSubscriptionId++;
  for (const kind of kinds) {
    if (!EVENT_SUBSCRIPTIONS.has(kind)) EVENT_SUBSCRIPTIONS.set(kind, new Set());
    EVENT_SUBSCRIPTIONS.get(kind).add(id);
  }
  return { subscription_id: id, kinds };
}

function unsubscribeEventsTool(args) {
  const id = Number(args.subscription_id);
  let removed = false;
  for (const subs of EVENT_SUBSCRIPTIONS.values()) {
    if (subs.delete(id)) removed = true;
  }
  return { subscription_id: id, removed };
}

function listEventTopicsTool() {
  return { topics: [...EVENT_TOPICS].sort() };
}

// FALTANTES item 5 (MCP server) — long operations with job id, progress,
// cancellation, timeout and artifacts. A build can run for minutes; the
// synchronous build_game would freeze the stdio loop, so start_build spawns
// the build asynchronously and returns a job id immediately. The caller polls
// build_status for progress (live log tail) and can cancel_build to kill the
// in-flight child. Jobs are kept in memory for the server's lifetime.
const BUILD_JOBS = new Map();
let nextBuildJobId = 1;

function startBuildTool(args) {
  const exe = String(args.exe ?? "VulkanEngineGame");
  const known = new Set(["VulkanEngineGame", "VulkanEngineEditor", "VulkanEngineServer",
    "VulkanEngineCooker", "vulkan_craft", "ALL_BUILD"]);
  if (!known.has(exe)) throw new Error(`unknown target '${exe}'`);
  const config = String(args.config ?? "Release");
  const buildRoot = path.join(ENGINE_ROOT, "build");
  if (!fs.existsSync(buildRoot)) throw new Error(`build dir not found: ${buildRoot}`);

  fs.mkdirSync(GAME_RUN_ROOT, { recursive: true });
  const jobId = nextBuildJobId++;
  const name = `build-${exe}-${new Date().toISOString().replace(/[:.]/g, "-")}.log`;
  const logPath = path.join(GAME_RUN_ROOT, name);
  const argsList = ["--build", "build", "--config", config];
  if (exe !== "ALL_BUILD") argsList.push("--target", exe);

  const job = {
    job_id: jobId,
    exe,
    config,
    status: "running",
    log: path.relative(ENGINE_ROOT, logPath).replaceAll(path.sep, "/"),
    started_at: new Date().toISOString(),
    exit_code: null,
    child: null
  };
  const child = spawn("cmake", argsList, { cwd: ENGINE_ROOT, windowsHide: true });
  job.child = child;
  const stream = fs.createWriteStream(logPath, { flags: "a" });
  child.stdout.on("data", (chunk) => stream.write(chunk));
  child.stderr.on("data", (chunk) => stream.write(chunk));
  child.on("error", (error) => {
    job.status = "error";
    job.error = error.message;
    job.finished_at = new Date().toISOString();
    stream.end();
    emitEvent("build.status_changed", { job_id: job.job_id, status: job.status, exe: job.exe, config: job.config });
  });
  child.on("exit", (code, signal) => {
    stream.end();
    if (job.status === "cancelled") return;  // cancel wins
    job.status = code === 0 ? "succeeded" : "failed";
    job.exit_code = code;
    job.signal = signal ?? null;
    job.finished_at = new Date().toISOString();
    emitEvent("build.status_changed", { job_id: job.job_id, status: job.status, exe: job.exe, config: job.config });
  });
  BUILD_JOBS.set(jobId, job);
  return {
    job_id: jobId,
    status: job.status,
    log: job.log,
    poll_with: "build_status"
  };
}

// task_plan Agente 5 §5 item 5 ("artefatos"): the artifacts a job produces.
// Deterministic: the log is always an artifact; for a SUCCEEDED single-target
// build the produced binary lives at build/<config>/<exe>.exe. Computed at
// read time so a job created before the binary existed still reports it.
// §5 item 5 ("bytes exatos"): each binary carries its real byte size, and the
// artifacts object exposes the total — real granular progress per target.
function jobArtifacts(job) {
  const artifacts = { log: job.log, binaries: [], bytes_total: 0 };
  if (job.status === "succeeded" && job.exe !== "ALL_BUILD") {
    const exePath = path.join(ENGINE_ROOT, "build", job.config, `${job.exe}.exe`);
    if (fs.existsSync(exePath)) {
      const rel = path.relative(ENGINE_ROOT, exePath).replaceAll(path.sep, "/");
      const bytes = fs.statSync(exePath).size;
      artifacts.binaries.push({ path: rel, bytes });
      artifacts.bytes_total += bytes;
    }
  }
  return artifacts;
}

// §5 item 5 ("progresso granular"): coarse build stage derived from the log
// tail markers (configure → compile → link), computed on read like artifacts.
// MSBuild/cmake markers: configure = re-running cmake / configuring done /
// build files written; compile = Building CXX / Building Custom Rule /
// Compiling; link = Linking / "-> .exe". Unknown/empty tail → "running".
function jobStage(tail) {
  if (!tail) return "running";
  const lines = tail.split(/\r?\n/).slice(-12).join("\n");
  if (/Linking\.\.\.|Linking CXX|-> .*\.(exe|dll)/.test(lines)) return "link";
  if (/Building CXX|Building Custom Rule|Compiling|MSBuild|-> .*\.vcxproj/.test(lines)) return "compile";
  if (/Re-running cmake|Configuring done|Build files have been written|CMake Configure|CMake Error/.test(lines)) return "configure";
  return "running";
}

// §5 item 5 ("progresso granular"): count of completed MSBuild targets from the
// FULL job log — MSBuild prints `  Target.vcxproj -> <build-dir>\Target.exe` for
// every completed target, so counting those lines is real granular progress.
function jobTargetsBuilt(logPath) {
  if (!fs.existsSync(logPath)) return 0;
  const log = fs.readFileSync(logPath, "utf8");
  return (log.match(/> .*\.(exe|dll|lib)/g) || []).length;
}

function buildStatusTool(args) {
  const jobId = Number(args.job_id);
  const job = BUILD_JOBS.get(jobId);
  if (!job) throw new Error(`unknown build job '${args.job_id}' (see list_build_jobs)`);
  const logPath = path.join(ENGINE_ROOT, job.log);
  const tail = fs.existsSync(logPath) ? tailLines(logPath, 60) : "";
  const lines = tail.split(/\r?\n/);
  return {
    job_id: job.job_id,
    status: job.status,
    exe: job.exe,
    config: job.config,
    started_at: job.started_at,
    finished_at: job.finished_at ?? null,
    exit_code: job.exit_code,
    signal: job.signal ?? null,
    error: job.error ?? null,
    log: job.log,
    stage: jobStage(tail),
    targets_built: jobTargetsBuilt(logPath),
    artifacts: jobArtifacts(job),
    tail
  };
}

function cancelBuildTool(args) {
  const job = BUILD_JOBS.get(Number(args.job_id));
  if (!job) throw new Error(`unknown build job '${args.job_id}' (see list_build_jobs)`);
  if (job.status !== "running") {
    return { job_id: job.job_id, cancelled: false, status: job.status };
  }
  job.status = "cancelled";
  job.finished_at = new Date().toISOString();
  const child = job.child;
  job.child = null;
  child?.kill();
  emitEvent("build.status_changed", { job_id: job.job_id, status: "cancelled", exe: job.exe, config: job.config });
  return { job_id: job.job_id, cancelled: true, status: "cancelled" };
}

function listBuildJobsTool() {
  const jobs = [...BUILD_JOBS.values()]
    .sort((a, b) => b.job_id - a.job_id)
    .map((job) => {
      const logPath = path.join(ENGINE_ROOT, job.log);
      const tail = fs.existsSync(logPath) ? tailLines(logPath, 60) : "";
      return {
        job_id: job.job_id,
        status: job.status,
        exe: job.exe,
        config: job.config,
        started_at: job.started_at,
        finished_at: job.finished_at ?? null,
        log: job.log,
        stage: jobStage(tail),
        artifacts: jobArtifacts(job)
      };
    });
  return { jobs };
}

const TOOLS = [
  ...semanticToolDefinitions(),
  ...publicContractTools(),
  ...controlApiToolDefinitions(),
  {
    name: "asset_cooker",
    description: "Expose the deterministic public asset cooking capability and cache contract.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "capability_registry",
    description: "Discover the public capability registry contract and its runtime metadata without exposing private implementation files.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "engine_overview",
    description: "Return a compact map of VulkanCraft engine modules, CMake targets, authoritative documents, and the concurrency rule.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "engine_pending_work",
    description: "Return only unchecked work items from docs/FALTANTES.md.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "engine_version",
    description: "Return the engine semantic version, ABI token and MCP server/protocol versions (single source of truth: engine/version.hpp).",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "list_directory",
    description: "List a scoped engine directory without dumping the whole repository.",
    inputSchema: {
      type: "object",
      properties: {
        path: { type: "string", default: "." },
        depth: { type: "integer", minimum: 0, maximum: 2, default: 1 },
        max_entries: { type: "integer", minimum: 1, maximum: 500, default: 120 }
      },
      additionalProperties: false
    }
  },
  {
    name: "search_code",
    description: "Search engine text/code with ripgrep-compatible regex or literal matching and bounded results.",
    inputSchema: {
      type: "object",
      required: ["query"],
      properties: {
        query: { type: "string", minLength: 1 },
        path: { type: "string", default: "src" },
        glob: { type: "string" },
        regex: { type: "boolean", default: false },
        case_sensitive: { type: "boolean", default: false },
        max_results: { type: "integer", minimum: 1, maximum: 300, default: 80 }
      },
      additionalProperties: false
    }
  },
  {
    name: "inspect_symbol",
    description: "Find declarations and uses of a C/C++ symbol in a scoped engine path.",
    inputSchema: {
      type: "object",
      required: ["symbol"],
      properties: {
        symbol: { type: "string", minLength: 1 },
        path: { type: "string", default: "src" },
        glob: { type: "string", default: "*.{h,hpp,cpp,cxx,cc}" },
        max_results: { type: "integer", minimum: 1, maximum: 300, default: 100 }
      },
      additionalProperties: false
    }
  },
  {
    name: "read_file",
    description: "Read a bounded line range from a text file and return its SHA-256 for safe concurrent editing.",
    inputSchema: {
      type: "object",
      required: ["path"],
      properties: {
        path: { type: "string" },
        start_line: { type: "integer", minimum: 1, default: 1 },
        end_line: { type: "integer", minimum: 1 }
      },
      additionalProperties: false
    }
  },
  {
    name: "apply_text_edits",
    description: "Atomically apply exact text replacements. Existing files require the SHA-256 returned by read_file, preventing lost updates between concurrent agents.",
    inputSchema: {
      type: "object",
      required: ["path", "expected_sha256", "edits"],
      properties: {
        path: { type: "string" },
        expected_sha256: { type: "string", pattern: "^[a-f0-9]{64}$" },
        dry_run: { type: "boolean", default: false },
        edits: {
          type: "array",
          minItems: 1,
          maxItems: 100,
          items: {
            type: "object",
            required: ["old_text", "new_text"],
            properties: {
              old_text: { type: "string", minLength: 1 },
              new_text: { type: "string" },
              replace_all: { type: "boolean", default: false }
            },
            additionalProperties: false
          }
        }
      },
      additionalProperties: false
    }
  },
  {
    name: "create_file",
    description: "Atomically create a text/code file under the engine root. Existing files are protected against accidental overwrite.",
    inputSchema: {
      type: "object",
      required: ["path", "content"],
      properties: {
        path: { type: "string" },
        content: { type: "string" },
        overwrite: { type: "boolean", default: false },
        expected_sha256: { type: "string", pattern: "^[a-f0-9]{64}$" },
        dry_run: { type: "boolean", default: false }
      },
      additionalProperties: false
    }
  },
  {
    name: "build_game",
    description: "Build a Release target (cmake --build) and store the full output in Projects/.runs; returns status, error lines and tail. Use before run_game when the exe is stale.",
    inputSchema: {
      type: "object",
      properties: {
        exe: { type: "string", default: "VulkanEngineGame", description: "Target: VulkanEngineGame, VulkanEngineEditor, VulkanEngineServer, VulkanEngineCooker, vulkan_craft or ALL_BUILD" },
        config: { type: "string", default: "Release" }
      },
      additionalProperties: false
    }
  },
  {
    name: "start_build",
    description: "Start a build asynchronously and return a job id immediately (the synchronous build_game blocks the server for the whole build). Poll build_status for progress and cancel_build to abort.",
    inputSchema: {
      type: "object",
      properties: {
        exe: { type: "string", default: "VulkanEngineGame", description: "Target: VulkanEngineGame, VulkanEngineEditor, VulkanEngineServer, VulkanEngineCooker, vulkan_craft or ALL_BUILD" },
        config: { type: "string", default: "Release" }
      },
      additionalProperties: false
    }
  },
  {
    name: "build_status",
    description: "Poll an async build job (start_build) for its status and live log tail.",
    inputSchema: {
      type: "object",
      required: ["job_id"],
      properties: { job_id: { type: "integer", minimum: 1 } },
      additionalProperties: false
    }
  },
  {
    name: "cancel_build",
    description: "Cancel a running build job (start_build); a finished job is reported but not re-cancelled.",
    inputSchema: {
      type: "object",
      required: ["job_id"],
      properties: { job_id: { type: "integer", minimum: 1 } },
      additionalProperties: false
    }
  },
  {
    name: "list_build_jobs",
    description: "List build jobs (start_build) newest first with their status and log path.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "audit_log",
    description: "Return the recent tool-call audit trail (tool, argument keys, error status, result hash) from this server process.",
    inputSchema: {
      type: "object",
      properties: { limit: { type: "integer", minimum: 1, maximum: 500, default: 100 } },
      additionalProperties: false
    }
  },
  {
    name: "subscribe_events",
    description: "Subscribe to server events (notifications/... messages) for builds, assets, scenes, game, tests, and profiler samples. Returns a subscription_id.",
    inputSchema: {
      type: "object",
      required: ["kinds"],
      properties: { kinds: { type: "array", items: { type: "string" }, description: "Event kinds to subscribe to" } },
      additionalProperties: false
    }
  },
  {
    name: "unsubscribe_events",
    description: "Unsubscribe a subscription (by id) and stop receiving its notifications.",
    inputSchema: {
      type: "object",
      required: ["subscription_id"],
      properties: { subscription_id: { type: "integer", minimum: 1 } },
      additionalProperties: false
    }
  },
  {
    name: "list_event_topics",
    description: "List the event kinds this server can emit for build, asset, scene, game, test, and profiler lifecycle changes.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "run_game",
    description: "Run a built engine executable for a bounded number of seconds, capture stdout/stderr to Projects/.runs/<exe>-<timestamp>.log and return the tail. The process is killed automatically at the timeout — no window is left open.",
    inputSchema: {
      type: "object",
      properties: {
        exe: { type: "string", default: "VulkanEngineGame", description: "VulkanEngineGame, VulkanEngineEditor, VulkanEngineServer, VulkanEngineCooker or vulkan_craft" },
        seconds: { type: "integer", minimum: 1, maximum: 120, default: 10 },
        args: { type: "array", items: { type: "string" }, description: "Extra CLI arguments passed to the executable" }
      },
      additionalProperties: false
    }
  },
  {
    name: "list_game_logs",
    description: "List captured run/build logs under Projects/.runs (newest first).",
    inputSchema: { type: "object", properties: {}, additionalProperties: false }
  },
  {
    name: "read_game_log",
    description: "Return the tail of a captured log from Projects/.runs (see list_game_logs).",
    inputSchema: {
      type: "object",
      required: ["log"],
      properties: {
        log: { type: "string" },
        lines: { type: "integer", minimum: 1, maximum: 2000, default: 100 }
      },
      additionalProperties: false
    }
  },
  {
    name: "package_game",
    description: "Publish a distributable package of a game project: runs VulkanPackageBuilder to stage Bin/<exe> + Content/ + PackageManifest.txt into Projects/<project>/Package (all-or-nothing; the exe must already be built via build_game).",
    inputSchema: {
      type: "object",
      required: ["project"],
      properties: {
        project: { type: "string" },
        exe: { type: "string", default: "VulkanEngineGame" },
        platform: { type: "string", default: "windows-x64" },
        configuration: { type: "string", default: "Shipping" }
      },
      additionalProperties: false
    }
  }
];

// FALTANTES item 5 (MCP server) — prompts: game-creation recipes that guide the
// LLM through the exact tool call sequence. Each prompt is a starting template;
// the LLM calls the tools directly (the prompt returns a message, not a tool
// result). Templates are grounded ONLY in real authoring tools (no UI/plugin
// tools exist yet — those surfaces are deferred).
const PROMPTS = [
  {
    name: "create_game_project",
    description: "Start a new VulkanCraft game project.",
    arguments: [{ name: "name", description: "project name (must be a valid directory name)", required: true }]
  },
  {
    name: "author_block",
    description: "Add a custom block type to the registry.",
    arguments: [{ name: "name", description: "block name (becomes the file name)", required: true }]
  },
  {
    name: "author_item",
    description: "Add a custom item type to the registry.",
    arguments: [{ name: "name", description: "item name (becomes the file name)", required: true }]
  },
  {
    name: "author_biome",
    description: "Add a custom biome to the world profile.",
    arguments: [{ name: "name", description: "biome name (human-readable id)", required: true }]
  },
  {
    name: "create_material",
    description: "Create a visual material (base color, roughness, metallic, emissive).",
    arguments: [{ name: "name", description: "material name", required: true }]
  },
  {
    name: "add_entity",
    description: "Add an entity with a component to a scene.",
    arguments: [{ name: "name", description: "entity name", required: true }]
  },
  {
    name: "author_ability",
    description: "Create an ability asset (damage, heal, impulse, flight, block edit, periodic).",
    arguments: [{ name: "name", description: "ability name", required: true }]
  },
  {
    name: "author_mission",
    description: "Create a mission asset with objectives, dialogue, and rewards.",
    arguments: [{ name: "name", description: "mission name", required: true }]
  },
  {
    name: "author_vehicle",
    description: "Create a vehicle asset with physics, wheels, and beam graph.",
    arguments: [{ name: "name", description: "vehicle name", required: true }]
  },
  {
    name: "create_plugin",
    description: "Register a plugin in a game project (create_game_project registers it; its assets are authored with the same author_* tools).",
    arguments: [{ name: "name", description: "plugin name", required: true }]
  },
  {
    name: "create_system",
    description: "Develop a new engine system (public contract + adapter + gate) using the source-maintenance tools (read_file/apply_text_edits/build).",
    arguments: [{ name: "name", description: "system name/domain", required: true }]
  },
  {
    name: "create_ui",
    description: "Author a UI screen document (UiDoc) in a game project, following the public IUiDoc contract (layout + widgets + viewport + confirmations).",
    arguments: [{ name: "name", description: "ui screen name (becomes the file name)", required: true }]
  }
];

function renderPrompt(name, args = {}) {
  const template = (s) => s.replace(/\{([a-zA-Z_]+)\}/g, (_, key) => args[key] ?? `{${key}}`);
  switch (name) {
    case "create_game_project":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Create a new VulkanCraft game project named "{name}". Steps:

1. Call \`create_game_project\` with project: "{name}".
2. Call \`build_game\` with project: "{name}" to compile the defaults.
3. Call \`run_game\` with project: "{name}" to verify it starts.

After the project exists, you can add blocks, items, entities, and scenes to it.`) }
        }]
      };

    case "author_block":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Create a custom block named "{name}". Steps:

1. Call \`author_registry_asset\` with kind: "block", name: "{name}", and the block fields:
   - \`hardness\` (number, >= 0): mining hardness.
   - \`collisionShape\` (string): "full", "cross", "slab", "stairs", "fence", "wall", "carpet", "none".
   - \`opaque\` (boolean): whether the block blocks light.
   - \`lightEmission\` (number, 0-15): emitted light level.
   - \`color\` (string): "R,G,B" or "R,G,B,A" in 0-255.
2. Call \`validate_game_project\` to verify the asset is valid.
3. Call \`build_game\` to compile the block into the game.`) }
        }]
      };

    case "author_item":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Create a custom item named "{name}". Steps:

1. Call \`author_registry_asset\` with kind: "item", name: "{name}", and the item fields:
   - \`stackSize\` (integer, >= 1): max stack size.
   - \`equipSlot\` (string): "none", "head", "chest", "legs", "feet", "hand", "offhand".
   - \`placeBlock\` (string): block id to place when used.
   - \`color\` (string): "R,G,B" or "R,G,B,A" in 0-255.
2. Call \`validate_game_project\` to verify the asset is valid.
3. Call \`build_game\` to compile the item into the game.`) }
        }]
      };

    case "author_biome":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Create a custom biome named "{name}". Steps:

1. Call \`author_world_profile_asset\` with name: "{name}" and a world profile document containing:
   - \`biomes\`: { biomes: [{ name: "{name}", engineBiomeIndex: <number> }] }.
   - Optional: \`height\`, \`climate\`, \`caves\`, \`ores\`, \`carver\`, \`decorators\`, \`structures\`.
2. Call \`validate_game_project\` to verify the profile is valid.
3. Call \`build_game\` to compile the profile into the game.`) }
        }]
      };

    case "create_material":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Create a visual material named "{name}". Steps:

1. Call \`create_material\` with name: "{name}" and the material fields:
   - \`ar\`, \`ag\`, \`ab\` (number, 0-1): base color.
   - \`roughness\` (number, 0-1): surface roughness.
   - \`metallic\` (number, 0-1): metalness.
   - \`er\`, \`eg\`, \`eb\` (number, 0-1): emissive color.
   - \`emissiveIntensity\` (number, >= 0): emissive brightness.
2. Call \`build_game\` to compile the material into the game.`) }
        }]
      };

    case "add_entity":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Add an entity named "{name}" to a scene. Steps:

1. Call \`create_entity\` with name: "{name}" to create the entity.
2. Call \`set_component\` with the entity id and the component you want:
   - \`Transform\` (px, py, pz, rx, ry, rz, sx, sy, sz) — position, rotation, scale.
   - \`MeshRenderer\` (mesh, material, visible, castShadows) — a visible mesh.
   - \`Rigidbody\` (mass, friction, restitution, kinematic, gravity) — physics.
   - \`Light\` (r, g, b, intensity, range, castShadows, type: 0=point).
   - \`Camera\` (fov, near, far, primary) — a camera.
   - \`ParticleEmitter\` (spawn rate, speed, life, color, gravity, etc.) — particles.
   - \`Audio\` (clip, volume, pitch, spatial, looping, playOnStart) — sound.
3. Call \`build_game\` to compile the entities into the game.`) }
        }]
      };

    case "author_ability":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Create an ability named "{name}". Steps:

1. Call \`author_ability_asset\` with name: "{name}" and the ability fields:
   - \`effects\` (required): array of effects. Each effect has a \`type\` (damage, heal, impulse, telekinesis, flight, blockEdit, periodic) and type-specific fields.
   - \`cooldownSeconds\` (number, >= 0): cooldown time.
   - \`targeting\` (object): { mode: self|direction|point|body, range, radius }.
   - \`cost\` (object): { resource: "mana"|"stamina", amount }.
   - \`conditions\` (array): [{ kind: ownerTag|targetTag|ownerAttribute|targetAttribute|distance, ... }].
2. Call \`validate_game_project\` to verify the asset is valid.
3. Call \`build_game\` to compile the ability into the game.`) }
        }]
      };

    case "author_mission":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Create a mission named "{name}". Steps:

1. Call \`author_mission_asset\` with name: "{name}" and the mission fields:
   - \`objectives\` (required): [{ id, kind: reach|collect|kill|interact, target, count, x, z, radius }].
   - \`dialogue\` (optional): [{ id (must include 'start'), speaker, text, choices: [{ text, next, conditions }] }].
   - \`reward\` (optional): { itemId, count, xp, setFlag }.
   - \`repeatable\` (boolean).
2. Call \`validate_game_project\` to verify the asset is valid.
3. Call \`build_game\` to compile the mission into the game.`) }
        }]
      };

    case "author_vehicle":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Create a vehicle named "{name}". Steps:

1. Call \`author_vehicle_asset\` with name: "{name}" and the vehicle fields:
   - \`enginePower\` (number): motor power.
   - \`mass\` (number): vehicle mass.
   - \`wheelRadius\` (number): wheel radius.
   - \`wheelBase\` (number): distance between front and rear axles.
   - \`track\` (number): distance between left and right wheels.
   - \`fwd\` (boolean): front-wheel drive.
2. Call \`validate_game_project\` to verify the asset is valid.
3. Call \`build_game\` to compile the vehicle into the game.`) }
        }]
      };

    case "create_plugin":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Register a plugin named "{name}" in a game project. Steps:

1. Call \`create_game_project\` with project name "{name}" (or the desired name) and \`plugins: ["{name}"]\` — the project manifest registers the plugin.
2. Author the plugin's assets under the project with the SAME tools the project uses:
   - \`author_registry_asset\` (blocks/items/fluids/recipes/biomes/structures).
   - \`author_ability_asset\`, \`author_mission_asset\`, \`author_vehicle_asset\`, \`author_gait_asset\`, \`author_world_profile_asset\`.
   - \`create_material\`, \`create_audio_event\`, \`create_physics_material\`, \`create_prefab\`, \`create_particle_asset\`.
   - \`create_visual_script\` for behavior graphs.
3. Call \`validate_game_project\` to verify everything is valid.
4. Call \`build_game\` to compile the project (including the plugin's assets).`) }
        }]
      };

    case "create_system":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Develop a new engine system named "{name}" using the source-maintenance tools (only when explicitly asked to develop the engine). Steps:

1. Call \`engine_overview\` and \`read_file\` on the public headers under \`src/engine/public/engine/\` for the "{name}" domain to learn the conventions (self-contained headers, #pragma once, namespace, all-or-nothing API).
2. Add the public contract header for "{name}" under \`src/engine/public/engine/\` with \`apply_text_edits\` (self-contained std, public API only).
3. Add the adapter under \`src/engine/sdk/\` and register it in the SDK target with \`apply_text_edits\`.
4. Add the gate test under \`tests/\` with \`apply_text_edits\`.
5. Call \`start_build\` (config Release) and poll \`build_status\` until the job finishes.
6. Fix any errors via \`read_file\` + \`apply_text_edits\`, then rebuild.`) }
        }]
      };

    case "create_ui":
      return {
        messages: [{
          role: "user",
          content: { type: "text", text: template(`Author a UI screen named "{name}" in a game project. Steps:

1. Call \`read_file\` on \`src/engine/public/engine/ui/IUiDoc.hpp\` (and \`ILayout.hpp\`, \`IWidgets.hpp\`, \`IViewport.hpp\`, \`IConfirmation.hpp\`) to learn the EXACT UiDoc JSON document shape — the public engine/ui contracts compose into ONE versioned document (layout tree + widgets + viewport + confirmations), and the header explicitly declares it the data surface for MCP tooling.
2. Write the UiDoc JSON to \`Content/UI/{name}.json\` in the game project with \`create_file\` (or \`apply_text_edits\` if the file exists), following the sub-contract field names and validation rules you read in step 1 (all-or-nothing: an invalid sub-document is rejected).
3. Call \`build_game\` with the project to compile the UI document into the game.`) }
        }]
      };

    default: throw new Error(`unknown prompt '${name}'`);
  }
}

const RESOURCE_MAP = new Map([
  ["engine://readme", "README.md"],
  ["engine://architecture", "docs/ARCHITECTURE.md"],
  ["engine://migration-status", "docs/MIGRATION_STATUS.md"],
  ["engine://pending-work", "docs/FALTANTES.md"],
  ["engine://sdk-manifest", "docs/SDK_MANIFEST.md"],
  ["engine://dependencies", "docs/SOLUCOES_E_DEPENDENCIAS.md"],
  ["engine://determinism", "docs/DETERMINISMO_PROVIDERS.md"],
  ["engine://dependency-policy", "docs/DEPENDENCY_POLICY.md"],
  // task_plan Agente 5 §5 item 3 ("métricas"): a DYNAMIC resource generated on
  // read — live server metrics (uptime, tool counts, audit ring, rate limit,
  // event subscriptions, SSE clients). The sentinel value "__metrics__" is
  // special-cased in resources/read (it is not a file).
  ["engine://metrics", "__metrics__"],
  // §5 item 3 ("projetos"): DYNAMIC resource — the live project list, same
  // enumeration as the list_game_projects tool (single source, no duplicate
  // walk). Sentinel "__projects__" is special-cased in resources/read.
  ["engine://projects", "__projects__"],
  // §5 item 3 ("tests"): DYNAMIC resource — the registered ctest tests of the
  // current build tree (ctest -N lists them WITHOUT executing — fast and
  // deterministic). Sentinel "__tests__" is special-cased in resources/read.
  ["engine://tests", "__tests__"]
]);

// FALTANTES item 5 (MCP server) — limits + audit: every tools/call is recorded
// in a bounded ring buffer (and appended to Projects/.runs/audit.jsonl) with its
// tool, argument keys (never the values — args can hold source text), error
// status, timestamp and result SHA. A per-process rate limit bounds the request
// rate so a runaway client cannot saturate the server (each stdio client spawns
// its own process, so the limit never affects other clients).
const AUDIT_LOG_PATH = path.join(GAME_RUN_ROOT, "audit.jsonl");
const AUDIT_RING = [];
const AUDIT_RING_MAX = 1000;
const RATE_LIMIT_PER_SECOND = 200;
const RATE_LIMIT_BURST = 400;
let rateTokens = RATE_LIMIT_BURST;
let rateLastRefill = Date.now();

function refillRateTokens() {
  const now = Date.now();
  const elapsed = (now - rateLastRefill) / 1000;
  rateTokens = Math.min(RATE_LIMIT_BURST, rateTokens + elapsed * RATE_LIMIT_PER_SECOND);
  rateLastRefill = now;
}

function consumeRateToken() {
  refillRateTokens();
  if (rateTokens < 1) throw new Error("rate limit exceeded; retry shortly");
  rateTokens -= 1;
}

function summarizeArgs(args) {
  if (args === null || typeof args !== "object") return [];
  return Object.keys(args);
}

function recordAudit(name, args, isError, result) {
  const entry = {
    ts: new Date().toISOString(),
    tool: name,
    args: summarizeArgs(args),
    is_error: Boolean(isError),
    result_sha256: sha256(String(result ?? ""))
  };
  AUDIT_RING.push(entry);
  if (AUDIT_RING.length > AUDIT_RING_MAX) AUDIT_RING.shift();
  try {
    fs.mkdirSync(GAME_RUN_ROOT, { recursive: true });
    fs.appendFileSync(AUDIT_LOG_PATH, `${JSON.stringify(entry)}\n`, "utf8");
  } catch {
    // auditing is best-effort; a full disk must not fail the tool call
  }
}

function auditLogTool(args) {
  const limit = Math.min(Math.max(Number(args.limit ?? 100), 1), 500);
  const entries = AUDIT_RING.slice(-limit).reverse();
  return {
    entries,
    count: entries.length,
    total_recorded: AUDIT_RING.length,
    rate: { per_second: RATE_LIMIT_PER_SECOND, burst: RATE_LIMIT_BURST }
  };
}

async function toolCall(name, args = {}) {
  consumeRateToken();
  const semanticResult = callSemanticTool(ENGINE_ROOT, name, args);
  if (semanticResult !== undefined) {
    const isError = Boolean(semanticResult && semanticResult.isError);
    recordAudit(name, args, isError, JSON.stringify(semanticResult));
    return { content: jsonText(semanticResult) };
  }
  const publicContractResult = callPublicRuntimeTool(ENGINE_ROOT, name, args);
  if (publicContractResult !== undefined) {
    recordAudit(name, args, false, JSON.stringify(publicContractResult));
    return { content: jsonText(publicContractResult) };
  }
  const controlApiResult = await callControlApiTool(name, args);
  if (controlApiResult !== undefined) {
    const isError = Boolean(controlApiResult && controlApiResult.isError);
    recordAudit(name, args, isError, JSON.stringify(controlApiResult));
    return { content: jsonText(controlApiResult) };
  }
  let result;
  switch (name) {
    case "engine_overview": result = engineOverview(); break;
    case "engine_pending_work": result = pendingStatus(); break;
    case "engine_version": result = versionTool(); break;
    case "capability_registry": result = { contract: "src/engine/public/engine/capabilities/ICapabilityRegistry.hpp", discoverable: true, formats: ["types", "components", "assets", "commands", "events", "services"] }; break;
    case "asset_cooker": result = { contract: "src/engine/public/engine/assets/IAssetCooker.hpp", deterministic: true, cache: "content-addressed" }; break;
    case "list_directory": result = walkDirectory(args.path ?? ".", Number(args.depth ?? 1), Number(args.max_entries ?? 120)); break;
    case "search_code": result = searchCode(args); break;
    case "inspect_symbol": result = inspectSymbol(args); break;
    case "read_file": result = readFileTool(args); break;
    case "apply_text_edits": result = applyTextEdits(args); break;
    case "create_file": result = createFile(args); break;
    case "build_game": result = buildGameTool(args); break;
    case "start_build": result = startBuildTool(args); break;
    case "build_status": result = buildStatusTool(args); break;
    case "cancel_build": result = cancelBuildTool(args); break;
    case "list_build_jobs": result = listBuildJobsTool(); break;
    case "subscribe_events": result = subscribeEventsTool(args); break;
    case "unsubscribe_events": result = unsubscribeEventsTool(args); break;
    case "list_event_topics": result = listEventTopicsTool(); break;
    case "audit_log": result = auditLogTool(args); break;
    case "run_game": result = await runGameTool(args); break;
    case "package_game": result = await packageGameTool(args); break;
    case "list_game_logs": result = listGameLogsTool(); break;
    case "read_game_log": result = readGameLogTool(args); break;
    default: throw new Error(`unknown tool '${name}'`);
  }
  recordAudit(name, args, false, JSON.stringify(result));
  return { content: jsonText(result) };
}

async function handleRequest(message) {
  const { id, method, params = {} } = message;
  if (method === "initialize") {
    const requested = params.protocolVersion ?? SUPPORTED_PROTOCOL_VERSION;
    if (!SUPPORTED_PROTOCOL_VERSIONS.includes(requested)) {
      return {
        jsonrpc: "2.0",
        id,
        error: {
          code: -32602,
          message: `unsupported protocol version '${requested}'`,
          data: { supportedProtocolVersions: SUPPORTED_PROTOCOL_VERSIONS }
        }
      };
    }
    return {
      jsonrpc: "2.0",
      id,
      result: {
        protocolVersion: requested,
        capabilities: { tools: { listChanged: false }, resources: { subscribe: false, listChanged: false }, prompts: { listChanged: false } },
        serverInfo: { name: SERVER_NAME, version: SERVER_VERSION, engineVersion: ENGINE_VERSION.string, engineAbi: ENGINE_VERSION.abi },
        instructions: "Create games through game_capabilities and the semantic game-authoring tools. Do not modify engine source code for supported capabilities. Use source-maintenance tools only when explicitly asked to develop the engine. For existing engine files, call read_file and pass its expected_sha256 to apply_text_edits."
      }
    };
  }
  if (method === "ping") return { jsonrpc: "2.0", id, result: {} };
  if (method === "prompts/list") {
    return {
      jsonrpc: "2.0",
      id,
      result: { prompts: PROMPTS.map(({ name, description, arguments: args }) => ({ name, description, arguments: args })) }
    };
  }
  if (method === "prompts/get") {
    try {
      const prompt = PROMPTS.find((p) => p.name === params.name);
      if (!prompt) throw new Error(`unknown prompt '${params.name}'`);
      const rendered = renderPrompt(prompt.name, params.arguments ?? {});
      return { jsonrpc: "2.0", id, result: { description: prompt.description, messages: rendered.messages } };
    } catch (error) {
      return { jsonrpc: "2.0", id, error: { code: -32602, message: error instanceof Error ? error.message : String(error) } };
    }
  }
  if (method === "tools/list") return { jsonrpc: "2.0", id, result: { tools: [...TOOLS, ...publicRuntimeTools()] } };
  if (method === "tools/call") {
    try {
      return { jsonrpc: "2.0", id, result: await toolCall(params.name, params.arguments ?? {}) };
    } catch (error) {
      recordAudit(params.name, params.arguments ?? {}, true, error instanceof Error ? error.message : String(error));
      return { jsonrpc: "2.0", id, result: errorResult(error) };
    }
  }
  if (method === "resources/list") {
    const resources = [...RESOURCE_MAP.entries()].map(([uri, file]) => ({
      uri,
      name: file,
      description: `VulkanCraft engine document: ${file}`,
      mimeType: "text/markdown"
    }));
    return { jsonrpc: "2.0", id, result: { resources } };
  }
  if (method === "resources/templates/list") {
    // §5 item 3 — dynamic per-project resources are advertised as URI
    // templates (MCP ResourceTemplate): the {name} parameter is resolved in
    // resources/read against the live project on disk.
    return {
      jsonrpc: "2.0",
      id,
      result: {
        resourceTemplates: [
          { uriTemplate: "engine://projects/{name}", name: "Game project details", mimeType: "application/json", description: "Live project inspection — same data as inspect_game_project (scenes, assets, prefabs, abilities, ...)" },
          { uriTemplate: "engine://projects/{name}/assets", name: "Game project assets", mimeType: "application/json", description: "Asset list of a game project" },
          { uriTemplate: "engine://projects/{name}/scenes", name: "Game project scenes", mimeType: "application/json", description: "Scene list of a game project" }
        ]
      }
    };
  }
  if (method === "resources/read") {
    try {
      // §5 item 3 — per-project dynamic resources: engine://projects/<name>
      // and engine://projects/<name>/(assets|scenes), grounded in the SAME
      // inspection as the inspect_game_project tool (single source of truth).
      const projectMatch = params.uri.match(/^engine:\/\/projects\/([^/]+)(?:\/(assets|scenes))?$/);
      if (projectMatch) {
        const projectName = decodeURIComponent(projectMatch[1]);
        const detail = projectMatch[2];
        const inspected = inspectProject(ENGINE_ROOT, projectName);  // throws if the project does not exist
        let payload;
        if (detail === "assets") payload = { project: inspected.project, assets: inspected.assets };
        else if (detail === "scenes") payload = { project: inspected.project, scenes: inspected.scenes };
        else payload = inspected;
        return {
          jsonrpc: "2.0",
          id,
          result: { contents: [{ uri: params.uri, mimeType: "application/json", text: JSON.stringify(payload, null, 2) }] }
        };
      }
      const file = RESOURCE_MAP.get(params.uri);
      if (!file) throw new Error(`unknown resource '${params.uri}'`);
      if (file === "__tests__") {
        // §5 item 3 ("tests") — dynamic tests resource: the registered ctest
        // tests of the current build tree. ctest -N lists without executing
        // (fast, deterministic per configured tree). If the tree is not
        // configured, the resource reports it instead of failing hard.
        const ctest = spawnSync("ctest", ["-N", "-C", "Release"], { cwd: path.join(ENGINE_ROOT, "build"), encoding: "utf8", windowsHide: true, timeout: 15000 });
        const tests = [];
        if (ctest.status === 0) {
          for (const m of ctest.stdout.matchAll(/Test\s+#\d+:\s+([^\r\n]+)/g)) tests.push(m[1].trim());
        }
        const payload = {
          build_configured: ctest.status === 0,
          count: tests.length,
          tests: tests.sort()
        };
        return {
          jsonrpc: "2.0",
          id,
          result: { contents: [{ uri: params.uri, mimeType: "application/json", text: JSON.stringify(payload, null, 2) }] }
        };
      }
      if (file === "__projects__") {
        // §5 item 3 — dynamic projects resource (no backing file): the same
        // enumeration as list_game_projects (imported — single source of
        // truth for project discovery). Deterministic: sorted by name.
        const projects = listProjects(ENGINE_ROOT);
        return {
          jsonrpc: "2.0",
          id,
          result: { contents: [{ uri: params.uri, mimeType: "application/json", text: JSON.stringify(projects, null, 2) }] }
        };
      }
      if (file === "__metrics__") {
        // §5 item 3 — dynamic metrics resource (no backing file): live server
        // state. Deterministic shape; uptime/subscriptions move with the
        // process, everything else is stable per server instance.
        let subscriptionCount = 0;
        for (const set of EVENT_SUBSCRIPTIONS.values()) subscriptionCount += set.size;
        const metrics = {
          server: "vulkancraft-engine",
          protocol_version: SUPPORTED_PROTOCOL_VERSION,
          transport: transportMode,
          uptime_seconds: Math.trunc((Date.now() - SERVER_START) / 1000),
          tools: { total: TOOLS.length, semantic: semanticToolDefinitions().length },
          audit_ring: { entries: AUDIT_RING.length, max: AUDIT_RING_MAX },
          rate_limit: { per_second: RATE_LIMIT_PER_SECOND, burst: RATE_LIMIT_BURST },
          event_subscriptions: subscriptionCount,
          sse_clients: sseClients.size
        };
        return {
          jsonrpc: "2.0",
          id,
          result: { contents: [{ uri: params.uri, mimeType: "application/json", text: JSON.stringify(metrics, null, 2) }] }
        };
      }
      const source = readText(file);
      return {
        jsonrpc: "2.0",
        id,
        result: { contents: [{ uri: params.uri, mimeType: "text/markdown", text: source.content }] }
      };
    } catch (error) {
      return { jsonrpc: "2.0", id, error: { code: -32002, message: error instanceof Error ? error.message : String(error) } };
    }
  }
  if (method === "shutdown") {
    shuttingDown = true;
    return { jsonrpc: "2.0", id, result: null };
  }
  if (method === "notifications/exit") {
    // stdio lifecycle: shutdown -> notifications/exit ends the process. Over
    // HTTP the server keeps serving other clients and stops via SIGINT/SIGTERM.
    if (shuttingDown && transportMode === "stdio") process.exit(0);
    return null;
  }
  if (method?.startsWith("notifications/")) return null;
  return { jsonrpc: "2.0", id, error: { code: -32601, message: `method not found: ${method}` } };
}

function send(message) {
  if (message !== null) process.stdout.write(`${JSON.stringify(message)}\n`);
}

let inputBuffer = "";
let contentLength = null;

function drainInput() {
  while (inputBuffer.length > 0) {
    if (contentLength !== null) {
      if (Buffer.byteLength(inputBuffer, "utf8") < contentLength) return;
      const bytes = Buffer.from(inputBuffer, "utf8");
      const body = bytes.subarray(0, contentLength).toString("utf8");
      inputBuffer = bytes.subarray(contentLength).toString("utf8").replace(/^\r?\n/, "");
      contentLength = null;
      dispatch(body);
      continue;
    }

    const headerMatch = inputBuffer.match(/^Content-Length:\s*(\d+)\r?\n\r?\n/i);
    if (headerMatch) {
      contentLength = Number(headerMatch[1]);
      inputBuffer = inputBuffer.slice(headerMatch[0].length);
      continue;
    }

    const newline = inputBuffer.indexOf("\n");
    if (newline < 0) return;
    const line = inputBuffer.slice(0, newline).trim();
    inputBuffer = inputBuffer.slice(newline + 1);
    if (line) dispatch(line);
  }
}

let shuttingDown = false;

function dispatch(serialized) {
  let message;
  try {
    message = JSON.parse(serialized);
  } catch (error) {
    send({ jsonrpc: "2.0", id: null, error: { code: -32700, message: `parse error: ${error.message}` } });
    return;
  }
  Promise.resolve(handleRequest(message)).then(send).catch((error) => {
    send({ jsonrpc: "2.0", id: message.id ?? null, error: { code: -32603, message: error.message } });
  });
}

process.on("uncaughtException", (error) => { log(error.stack ?? error.message); process.exit(1); });
process.on("unhandledRejection", (error) => { log(error?.stack ?? String(error)); process.exit(1); });

// ---- optional remote transport (FALTANTES item 5) ----
function respondJson(res, status, payload) {
  const body = JSON.stringify(payload ?? { jsonrpc: "2.0", id: null, result: null });
  res.writeHead(status, {
    "content-type": "application/json",
    "content-length": Buffer.byteLength(body, "utf8"),
    "cache-control": "no-store"
  });
  res.end(body);
}

function handleHttpRequest(req, res) {
  let body = "";
  req.setEncoding("utf8");
  req.on("data", (chunk) => {
    body += chunk;
    if (Buffer.byteLength(body, "utf8") > MAX_TEXT_BYTES) {
      respondJson(res, 413, { jsonrpc: "2.0", id: null, error: { code: -32600, message: "request too large" } });
      req.destroy();
    }
  });
  req.on("end", () => {
    let message;
    try {
      message = JSON.parse(body.length > 0 ? body : "{}");
    } catch (error) {
      respondJson(res, 200, { jsonrpc: "2.0", id: null, error: { code: -32700, message: `parse error: ${error.message}` } });
      return;
    }
    Promise.resolve(handleRequest(message))
      .then((reply) => respondJson(res, 200, reply ?? { jsonrpc: "2.0", id: message.id ?? null, result: null }))
      .catch((error) => respondJson(res, 200, { jsonrpc: "2.0", id: message.id ?? null, error: { code: -32603, message: error.message } }));
  });
}

function handleSse(req, res) {
  res.writeHead(200, {
    "content-type": "text/event-stream",
    "cache-control": "no-store",
    "connection": "keep-alive"
  });
  res.write(": connected\n\n");
  sseClients.add(res);
  req.on("close", () => sseClients.delete(res));
}

function startHttpTransport(port, host) {
  transportMode = "http";
  httpServer = http.createServer((req, res) => {
    if (AUTH_TOKEN && (req.headers.authorization ?? "") !== `Bearer ${AUTH_TOKEN}`) {
      // §5 item 7: token auth enabled -> unauthorized requests rejected at the
      // transport level before any JSON-RPC handling (POST gets a JSON-RPC
      // server error, SSE gets an HTTP 401).
      if (req.method === "POST") {
        return respondJson(res, 401, { jsonrpc: "2.0", id: null, error: { code: -32001, message: "unauthorized: missing or invalid MCP_AUTH_TOKEN" } });
      }
      res.writeHead(401, { "content-type": "text/plain" });
      res.end("unauthorized: missing or invalid MCP_AUTH_TOKEN\n");
      return;
    }
    const url = new URL(req.url ?? "/", `http://${host}:${port}`);
    if (req.method === "GET" && url.pathname === "/events") return handleSse(req, res);
    if (req.method === "POST" && (url.pathname === "/mcp" || url.pathname === "/")) return handleHttpRequest(req, res);
    respondJson(res, 404, { jsonrpc: "2.0", id: null, error: { code: -32601, message: `not found: ${req.method} ${url.pathname}` } });
  });
  httpServer.on("connection", (socket) => socket.setTimeout(120000));
  httpServer.listen(port, host, () => log(`ready (http); root=${ENGINE_ROOT}; listening on http://${host}:${port}`));
}

function installSignalHandlers() {
  const stop = (sig) => {
    log(`${sig} received; shutting down`);
    if (transportMode === "http" && httpServer) {
      for (const res of sseClients) { try { res.end(); } catch { /* ignore */ } }
      httpServer.close(() => process.exit(0));
      setTimeout(() => process.exit(0), 2000).unref();
    } else {
      process.exit(0);
    }
  };
  process.on("SIGINT", () => stop("SIGINT"));
  process.on("SIGTERM", () => stop("SIGTERM"));
}
installSignalHandlers();

const argv = process.argv.slice(2);
const httpFlag = argv.indexOf("--http");
if (httpFlag >= 0) {
  const portFlag = argv.indexOf("--port");
  const port = portFlag >= 0 && Number(argv[portFlag + 1]) > 0 ? Number(argv[portFlag + 1]) : 8322;
  startHttpTransport(port, "127.0.0.1");
} else {
  process.stdin.setEncoding("utf8");
  process.stdin.on("data", (chunk) => { inputBuffer += chunk; drainInput(); });
  process.stdin.on("end", () => process.exit(0));
  log(`ready; root=${ENGINE_ROOT}`);
}
