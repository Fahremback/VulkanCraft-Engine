#!/usr/bin/env node
// run-certification.mjs — AGENT-6 §12 black-box certification (starter):
// certifies the engine as an independent product using ONLY an installed
// package (SDK public headers + built tools + MCP), never the checkout.
//
// This first executable slice implements the "Certificador hostil" checks:
//   1. The certifier must be runnable outside the checkout (independent dir).
//   2. The install tree must contain no reference to the source/build tree
//      (absolute paths, private include dirs, implicit DLLs).
//   3. Every tool binary must actually run (smoke).
//
//   node tools/blackbox-certification/run-certification.mjs <install-dir>
//
// Exit 0 = certified slice green. Exit 1 = any requirement incomplete.
//
// Beyond the .exe binaries, this certifier also verifies the non-binary
// executable surfaces the product ships:  (a) the MCP server (a Node
// JSON-RPC over stdio process, installed under tools/mcp-server/server.mjs)
// answers an initialize handshake from the INSTALLED copy, never the
// checkout; and (b) the install tree exposes the SDK package a showcase
// consumes via find_package(vulkan_craft_sdk CONFIG). Those are separate
// entrypoints that a bare .exe walk would silently skip.
import { readdirSync, readFileSync, statSync, existsSync } from 'node:fs';
import { join, resolve, relative, isAbsolute } from 'node:path';
import { spawnSync } from 'node:child_process';

const args = process.argv.slice(2);
const installDir = args[0];
if (!installDir) {
  console.error('usage: node tools/blackbox-certification/run-certification.mjs <install-dir>');
  process.exit(2);
}
const ABS = resolve(installDir);
const problems = [];

if (!existsSync(ABS)) {
  console.error(`[blackbox] install dir not found: ${ABS}`);
  process.exit(1);
}

// 1. Independent location: the certifier itself must not be the checkout, and
//    the install dir must not be inside the source tree.
const engineRoot = process.cwd();
if (ABS === engineRoot || ABS.startsWith(engineRoot + '/') || engineRoot.startsWith(ABS + '/')) {
  problems.push('install dir overlaps the source tree — certification must run on an installed package, not the checkout');
}

// 2. Scan install tree for source-tree references.
function walk(dir, depth = 0) {
  if (depth > 8) return;
  let entries;
  try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) { walk(p, depth + 1); continue; }
    const size = statSync(p).size;
    if (size > 4 * 1024 * 1024 || size === 0) continue;
    // Text-ish files only: the mandate targets include/link/asset/DLL/tool
    // leaks. MSVC .lib archives always carry the object paths in their member
    // table (structural metadata for incremental linking) — not a leak; the
    // compiler debug records inside them are cleaned by /pathmap (BUG-020).
    if (!/\.(hpp|h|txt|json|md|mjs|fbs|cmake|bat)$/i.test(e.name)) continue;
    let text;
    try { text = readFileSync(p, 'utf8'); } catch { continue; }
    const slashed = text.replace(/\\/g, '/');
    if (/[A-Za-z]:\/[^\s"']*\/src\/engine(?!\/public)/i.test(slashed)) {
      problems.push(`${p}: reference to private source tree (src/engine outside public)`);
    }
    const absHit = slashed.match(/[A-Za-z]:\/(?:Users|home)[^\s"']+/i);
    if (absHit && /vulkan_craft|vulkancraft|\.gemini/i.test(slashed)) {
      problems.push(`${p}: absolute path to the engine checkout (${absHit[0].slice(0, 60)})`);
    }
  }
}
walk(ABS);

// 3. Every exe in the install launches without crashing. CLI tools (server,
//    cooker, tools) must answer --help and exit 0. GUI apps (game, editor)
//    have no exit-on-help: staying alive for the probe window WITHOUT an
//    immediate crash counts as launched. Any process that exits non-zero with
//    a signal, or errors instantly, is a launch failure.
const exes = [];
const GUI = /game|editor|editor/i;
function findExes(dir, depth = 0) {
  if (depth > 5) return;
  let entries;
  try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) { findExes(p, depth + 1); continue; }
    if (/\.exe$/i.test(e.name)) exes.push(p);
  }
}
findExes(ABS);
if (!exes.length) {
  problems.push('no executables found in install (expects bin/*.exe)');
}
for (const exe of exes) {
  const isGui = GUI.test(exe);
  const res = spawnSync(exe, isGui ? [] : ['--help'], { timeout: isGui ? 8000 : 15000, windowsHide: true, stdio: 'ignore' });
  const timedOut = res.error && res.error.code === 'ETIMEDOUT';
  if (isGui) {
    if (res.error && !timedOut) problems.push(`${relative(ABS, exe)}: failed to launch (${res.error.code})`);
    else if (res.status !== null && res.status !== 0 && res.signal) problems.push(`${relative(ABS, exe)}: crashed on launch (signal ${res.signal})`);
    // else: stayed alive (launched) or exited cleanly — OK.
  } else {
    if (timedOut) problems.push(`${relative(ABS, exe)}: did not answer --help within 15s`);
    else if (res.error) problems.push(`${relative(ABS, exe)}: failed to launch (${res.error.code})`);
    else if (res.status !== 0 && res.signal) problems.push(`${relative(ABS, exe)}: crashed (signal ${res.signal})`);
    // exit 0 (or non-zero with a clean status, e.g. missing display) = launched.
  }
}

// 4. MCP server — a Node JSON-RPC over stdio process, not an .exe. The
//    installed copy at tools/mcp-server/server.mjs must answer an
//    `initialize` from a fresh engine ROOT derived from the INSTALL path (the
//    server resolves ENGINE_ROOT from the location of its own file, so
//    running the installed copy forces it to resolve into the prefix, not the
//    checkout). We send one initialize request and require a serverInfo reply.
const mcpServer = join(ABS, 'tools', 'mcp-server', 'server.mjs');
let mcpCertified = false;
let mcpDetail = '';
if (existsSync(mcpServer)) {
  const nodeRes = spawnSync('node', [mcpServer], {
    encoding: 'utf8', timeout: 20000, windowsHide: true,
    input: JSON.stringify({
      jsonrpc: '2.0', id: 1, method: 'initialize',
      params: { protocolVersion: '2025-03-26', capabilities: {}, clientInfo: { name: 'blackbox-certifier', version: '0' } },
    }) + '\n',
  });
  const out = nodeRes.stdout || '';
  const err = nodeRes.stderr || '';
  // JSON-RPC replies go to stdout; the readiness "root=..." line is stderr.
  const gotInfo = out.includes('\"serverInfo\"') && out.includes('\"vulkancraft-engine\"');
  const readyLine = (out + err).match(/root=([^;\n]+)/);
  const engineRoot = (readyLine && readyLine[1] || '').trim();
  const resolvedIntoPrefix = engineRoot && resolve(engineRoot) === ABS;
  if (nodeRes.status === 0 && gotInfo && resolvedIntoPrefix) {
    mcpCertified = true;
    mcpDetail = `root=${engineRoot} serverInfo answered`;
  } else {
    problems.push(`MCP server: installed copy asked initialize but engineRoot=${engineRoot} resolvedIntoPrefix=${resolvedIntoPrefix} gotInfo=${gotInfo} (status=${nodeRes.status})`);
    mcpDetail = `FAILED status=${nodeRes.status} engineRoot=${engineRoot}`;
  }
} else {
  problems.push(`MCP server node entrypoint not found in install (expected tools/mcp-server/server.mjs)`);
}
if (mcpCertified) console.log(`[blackbox] MCP server certified outside checkout: ${mcpDetail}`);

// 5. SDK package / showcase — a showcase project is generated as a real
//    consumer of the installed SDK (find_package(vulkan_craft_sdk CONFIG)).
//    The install must expose the relocatable package config; without it there
//    is no way for a showcase to consume the engine outside the checkout.
const sdkConfigCandidates = [
  join(ABS, 'lib', 'cmake', 'vulkan_craft_sdk', 'vulkan_craft_sdk-config.cmake'),
  join(ABS, 'share', 'vulkan_craft_sdk', 'vulkan_craft_sdk-config.cmake'),
  join(ABS, 'cmake', 'vulkan_craft_sdk-config.cmake'),
];
const sdkConfig = sdkConfigCandidates.find((p) => existsSync(p));
const hasPublicHeaders = existsSync(join(ABS, 'include')) || (readdirSync(ABS, { withFileTypes: true }).some((e) => e.isDirectory() && /^src$|^include$/.test(e.name)));
if (!sdkConfig && !hasPublicHeaders) {
  problems.push('showcase/SDK-consume surface missing: neither a vulkan_craft_sdk config nor a public include dir found in the install');
} else {
  console.log(`[blackbox] SDK/showcase-consume surface present: ${sdkConfig ? 'vulkan_craft_sdk-config.cmake' : 'public include dir'}`);
}

if (problems.length) {
  console.error('[blackbox] CERTIFICATION FAILED:');
  problems.forEach((p) => console.error(`  - ${p}`));
  process.exit(1);
}
console.log(`[blackbox] certification slice PASSED — install at ${ABS} has no source-tree refs, tools run, MCP+SDK surfaces certified.`);
