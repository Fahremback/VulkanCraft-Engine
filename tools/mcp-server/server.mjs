#!/usr/bin/env node

import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { callSemanticTool, semanticToolDefinitions } from "./game-authoring.mjs";
import { callControlApiTool, controlApiToolDefinitions } from "./control-api.mjs";

const SERVER_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(SERVER_DIR, "..", "..");
const SERVER_NAME = "vulkancraft-engine";
const SERVER_VERSION = "0.1.0";
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
  const targets = [...cmake.matchAll(/add_(?:executable|library)\s*\(\s*([^\s\)]+)/g)].map((match) => match[1]);
  return {
    root: ENGINE_ROOT,
    architecture: "Runtime + Editor + Tools + Plugins + Projects",
    modules,
    cmake_targets: targets,
    authoritative_docs: ["README.md", "docs/ARCHITECTURE.md", "docs/MIGRATION_STATUS.md", "docs/FALTANTES.md"],
    concurrency_rule: "Read before edit and pass expected_sha256 to every edit of an existing file."
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
  const result = spawnSync("cmake", argsList, { cwd: ENGINE_ROOT, encoding: "utf8", windowsHide: true, maxBuffer: 32 * 1024 * 1024 });
  const output = `${result.stdout ?? ""}${result.stderr ?? ""}`;
  fs.writeFileSync(logPath, output, "utf8");
  const lines = output.split(/\r?\n/);
  const errors = lines.filter((line) => /\berror\b/i.test(line)).slice(0, 20);
  return {
    target: exe,
    log: path.relative(ENGINE_ROOT, logPath).replaceAll(path.sep, "/"),
    status: result.status,
    errors: errors.length,
    error_lines: errors,
    tail: lines.slice(-60).join("\n")
  };
}

const TOOLS = [
  ...semanticToolDefinitions(),
  ...controlApiToolDefinitions(),
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

    default: throw new Error(`unknown prompt '${name}'`);
  }
}

const RESOURCE_MAP = new Map([
  ["engine://readme", "README.md"],
  ["engine://architecture", "docs/ARCHITECTURE.md"],
  ["engine://migration-status", "docs/MIGRATION_STATUS.md"],
  ["engine://pending-work", "docs/FALTANTES.md"]
]);

async function toolCall(name, args = {}) {
  const semanticResult = callSemanticTool(ENGINE_ROOT, name, args);
  if (semanticResult !== undefined) return { content: jsonText(semanticResult) };
  const controlApiResult = await callControlApiTool(name, args);
  if (controlApiResult !== undefined) return { content: jsonText(controlApiResult) };
  switch (name) {
    case "engine_overview": return { content: jsonText(engineOverview()) };
    case "engine_pending_work": return { content: jsonText(pendingStatus()) };
    case "list_directory": return { content: jsonText(walkDirectory(args.path ?? ".", Number(args.depth ?? 1), Number(args.max_entries ?? 120))) };
    case "search_code": return { content: jsonText(searchCode(args)) };
    case "inspect_symbol": return { content: jsonText(inspectSymbol(args)) };
    case "read_file": return { content: jsonText(readFileTool(args)) };
    case "apply_text_edits": return { content: jsonText(applyTextEdits(args)) };
    case "create_file": return { content: jsonText(createFile(args)) };
    case "build_game": return { content: jsonText(buildGameTool(args)) };
    case "run_game": return { content: jsonText(await runGameTool(args)) };
    case "list_game_logs": return { content: jsonText(listGameLogsTool()) };
    case "read_game_log": return { content: jsonText(readGameLogTool(args)) };
    default: throw new Error(`unknown tool '${name}'`);
  }
}

async function handleRequest(message) {
  const { id, method, params = {} } = message;
  if (method === "initialize") {
    return {
      jsonrpc: "2.0",
      id,
      result: {
        protocolVersion: params.protocolVersion ?? "2025-03-26",
        capabilities: { tools: { listChanged: false }, resources: { subscribe: false, listChanged: false }, prompts: { listChanged: false } },
        serverInfo: { name: SERVER_NAME, version: SERVER_VERSION },
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
  if (method === "tools/list") return { jsonrpc: "2.0", id, result: { tools: TOOLS } };
  if (method === "tools/call") {
    try {
      return { jsonrpc: "2.0", id, result: await toolCall(params.name, params.arguments ?? {}) };
    } catch (error) {
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
  if (method === "resources/read") {
    try {
      const file = RESOURCE_MAP.get(params.uri);
      if (!file) throw new Error(`unknown resource '${params.uri}'`);
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

process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => { inputBuffer += chunk; drainInput(); });
process.stdin.on("end", () => process.exit(0));
process.on("uncaughtException", (error) => { log(error.stack ?? error.message); process.exit(1); });
process.on("unhandledRejection", (error) => { log(error?.stack ?? String(error)); process.exit(1); });

log(`ready; root=${ENGINE_ROOT}`);
