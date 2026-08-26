#!/usr/bin/env node
// build-dir-usage.mjs — Agent 6 §9/146: detect which processes are actively
// using each build directory, so cleanup of legacy/in-source build trees is
// gated on real evidence instead of guessing (never interrupt concurrent work).
//
//   node tools/portability/build-dir-usage.mjs
//
// Output: for each candidate dir, the list of running processes whose
// executable path, command line, or current directory references it.
import { execSync } from 'node:child_process';
import { existsSync, readdirSync } from 'node:fs';
import { join, resolve, normalize } from 'node:path';

const root = process.cwd();

// Candidate build/artifact dirs in the source tree.
const candidates = ['build', 'build-asan', 'Intermediate', 'out'];
try {
  for (const e of readdirSync(root, { withFileTypes: true })) {
    if (e.isDirectory() && /\.dir$/.test(e.name)) candidates.push(e.name);
  }
} catch { /* ignore */ }

const targetSet = new Set(candidates.map((c) => normalize(resolve(root, c)).toLowerCase()));

function ps() {
  // Win32_Process gives ExecutablePath, CommandLine, CurrentDirectory.
  const cmd = `powershell -NoProfile -Command "Get-CimInstance Win32_Process | Select-Object ProcessId,Name,ExecutablePath,CommandLine,CurrentDirectory | ConvertTo-Json -Compress"`;
  try {
    const out = execSync(cmd, { encoding: 'utf8', timeout: 30000, windowsHide: true });
    const data = JSON.parse(out.trim());
    return Array.isArray(data) ? data : [data];
  } catch (e) {
    console.error(`[build-dir-usage] could not query processes: ${e.message}`);
    return [];
  }
}

function usedBy(proc, target) {
  const fields = [proc.ExecutablePath, proc.CommandLine, proc.CurrentDirectory];
  return fields.some((f) => f && f.toLowerCase().includes(target.toLowerCase()));
}

const procs = ps();
let any = false;
for (const c of candidates) {
  const abs = normalize(resolve(root, c)).toLowerCase();
  if (!existsSync(resolve(root, c))) continue;
  const users = procs.filter((p) => usedBy(p, abs) || usedBy(p, c.toLowerCase()));
  const names = users.map((p) => `PID ${p.ProcessId} ${p.Name} [${p.CommandLine ? p.CommandLine.slice(0, 90) : 'n/a'}]`);
  if (names.length) {
    any = true;
    console.log(`IN-USE  ${c} (${abs})`);
    for (const n of names) console.log(`        ${n}`);
  } else {
    console.log(`IDLE    ${c}`);
  }
}
if (!any) console.log('[build-dir-usage] no build dir currently referenced by a running process');
