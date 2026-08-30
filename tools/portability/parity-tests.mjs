#!/usr/bin/env node
// parity-tests.mjs — Contract parity tests (§11-22): the SAME operation must
// behave identically across C++ (semantic_api_tests.exe), CLI (semantic-cli
// factories = callSemanticTool) and MCP (server.mjs stdio transport).
// Exit 0 = parity OK; 1 = any surface diverged or failed.
import { readFileSync, existsSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { spawnSync } from 'child_process';
import { callSemanticTool, semanticToolDefinitions } from '../mcp-server/game-authoring.mjs';
import { ERRORS } from './error-registry.mjs';

const ROOT = process.cwd();
const DEFS = semanticToolDefinitions();

// ---- Stable error codes (must be referenced to keep error-registry green) ----
const ERR_PARITY = ERRORS.find((e) => e.id === 'E-3002'); // CLI op failed (isError)
const ERR_TOOL = ERRORS.find((e) => e.id === 'E-1003');   // unknown tool
const ERR_REFUSED = ERRORS.find((e) => e.id === 'E-1002'); // operation refused (validation)

function fail(msg) {
  console.error('PARITY FAIL: ' + msg);
  process.exitCode = 1;
}

// ---- Surface 1: C++ (semantic_api_tests.exe must pass) ----
function cppSurface() {
  const candidates = [
    // Canonical shared tree first (single-config Ninja, binaries at root),
    // then multi-config subdirs, then legacy in-tree fallbacks.
    join(ROOT, process.env.VC_BUILD_DIR || 'out/dev-shared', 'semantic_api_tests.exe'),
    join(ROOT, process.env.VC_BUILD_DIR || 'out/dev-shared', 'Release', 'semantic_api_tests.exe'),
    join(ROOT, 'build', 'Release', 'semantic_api_tests.exe'),
    join(ROOT, 'out', 'release', 'bin', 'semantic_api_tests.exe'),
  ];
  const exe = candidates.find((p) => existsSync(p));
  if (!exe) {
    fail('C++ semantic_api_tests.exe not found (build it first)');
    return false;
  }
  const res = spawnSync(exe, [], { encoding: 'utf8', timeout: 120000, windowsHide: true });
  const ok = res.status === 0 && /ALL PASSED/.test(res.stdout || '');
  console.log(`[C++] semantic_api_tests ${ok ? 'PASSED' : 'FAILED'} (exit ${res.status})`);
  if (!ok) fail(`C++ surface: ${(res.stdout || '') + (res.stderr || '')}`.slice(0, 400));
  return ok;
}

// ---- Surface 2: CLI factories (callSemanticTool) ----
function cliSurface(toolName, args) {
  const def = DEFS.find((d) => d.name === toolName);
  if (!def) {
    fail(`CLI: unknown tool ${toolName} (${ERR_TOOL.id})`);
    return null;
  }
  let result;
  try {
    result = callSemanticTool(ROOT, toolName, args);
  } catch (error) {
    result = { isError: true, message: error instanceof Error ? error.message : String(error) };
  }
  if (result === undefined) {
    fail(`CLI: tool ${toolName} returned no result (${ERR_PARITY.id})`);
    return null;
  }
  return result;
}

// ---- Surface 3: MCP stdio transport ----
function mcpSurface(toolName, args) {
  const server = join(ROOT, 'tools', 'mcp-server', 'server.mjs');
  const child = spawnSync(process.execPath, [server], {
    cwd: ROOT,
    encoding: 'utf8',
    timeout: 60000,
    windowsHide: true,
    input: JSON.stringify({
      jsonrpc: '2.0', id: 1, method: 'tools/call',
      params: { name: toolName, arguments: args },
    }) + '\n',
  });
  // Server prints ready line + JSON response on stdout
  const out = child.stdout || '';
  const lines = out.split(/\r?\n/).filter((l) => l.trim().startsWith('{'));
  let response = null;
  for (const line of lines) {
    try {
      const parsed = JSON.parse(line);
      if (parsed.id === 1 && parsed.result !== undefined) response = parsed.result;
    } catch { /* keep scanning */ }
  }
  if (!response) {
    fail(`MCP: no response for ${toolName} (${(child.stderr || '').slice(0, 200)})`);
    return null;
  }
  // MCP result: { content: [{ type: "text", text: "JSON" }], isError? }
  if (response.content && response.content[0] && response.content[0].text) {
    const text = response.content[0].text;
    try {
      const inner = JSON.parse(text);
      return { ...response, parsed: inner };
    } catch {
      return { ...response, parsed: text };
    }
  }
  return response;
}

// Normalize for comparison: drop volatile fields (timestamps, paths, counts)
function normalize(value) {
  if (Array.isArray(value)) return value.map(normalize);
  if (value !== null && typeof value === 'object') {
    const out = {};
    for (const [k, v] of Object.entries(value)) {
      if (/time|path|root|uptime|audit|sha|size|count|generated|updated|created/i.test(k)) continue;
      out[k] = normalize(v);
    }
    return out;
  }
  return value;
}

function compare(label, a, b) {
  const na = JSON.stringify(normalize(a));
  const nb = JSON.stringify(normalize(b));
  const ok = na === nb;
  console.log(`[parity] ${label}: ${ok ? 'MATCH' : 'DIVERGE'}`);
  if (!ok) {
    console.error('  CLI : ' + na.slice(0, 300));
    console.error('  MCP : ' + nb.slice(0, 300));
    fail(`${label} diverged between CLI and MCP`);
  }
  return ok;
}

function main() {
  const tools = process.argv.slice(2);
  const names = tools.length ? tools : ['game_capabilities', 'list_game_projects'];
  console.log(`# Contract parity tests (§11-22) — surfaces: C++, CLI(factories), MCP(stdio)\n`);

  let allOk = cppSurface();

  for (const name of names) {
    if (!DEFS.find((d) => d.name === name)) {
      console.log(`[skip] unknown semantic tool '${name}'`);
      continue;
    }
    const args = {};
    const cli = cliSurface(name, args);
    const mcp = mcpSurface(name, args);
    if (!cli || !mcp) continue;
    // CLI result: { isError?, ... } — MCP parsed: same JSON document
    compare(`${name} (CLI vs MCP)`, cli, mcp && mcp.parsed !== undefined ? mcp.parsed : mcp);
    const cliErr = Boolean(cli && cli.isError);
    const mcpErr = Boolean(mcp && (mcp.isError === true || (mcp.parsed && mcp.parsed.isError)));
    if (cliErr !== mcpErr) {
      fail(`${name}: isError flag diverged (CLI=${cliErr} MCP=${mcpErr})`);
      allOk = false;
    }
    if (cliErr) {
      // both surfaces must refuse consistently (E-1002 — operation refused)
      const cliRefuse = /refus|invalid|não|nao|unknown|not found|fail/i.test(cli.message || cli.error || '');
      const mcpRefuse = /refus|invalid|não|nao|unknown|not found|fail/i.test((mcp.parsed && (mcp.parsed.message || mcp.parsed.error)) || mcp.error || '');
      if (cliRefuse !== mcpRefuse) {
        fail(`${name}: refusal reason shape diverged (${ERR_REFUSED.id})`);
        allOk = false;
      } else {
        console.log(`[parity] ${name}: both surfaces refuse consistently (${ERR_REFUSED.id})`);
      }
    }
  }

  if (process.exitCode === 1) {
    console.error('\nPARITY TESTS FAILED');
    process.exit(1);
  }
  console.log('\nPARITY TESTS PASSED — C++ + CLI + MCP agree on the same operations.');
}

main();
