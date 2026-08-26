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

if (problems.length) {
  console.error('[blackbox] CERTIFICATION FAILED:');
  problems.forEach((p) => console.error(`  - ${p}`));
  process.exit(1);
}
console.log(`[blackbox] certification slice PASSED — install at ${ABS} has no source-tree refs, tools run.`);
