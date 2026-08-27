#!/usr/bin/env node
// mingw-env.mjs - shared helper for portability gates that compile+run MinGW g++ probes.
// Problem: a MinGW UCRT exe needs libstdc++/libgcc/libwinpthread DLLs at load time, and the
// gate process may not have the toolchain bin dir in PATH. Two facts make gates fragile:
//   (1) g++ itself may be missing from a stripped/CI PATH.
//   (2) the produced .exe fails to LOAD with exit 0xC0000139 if the DLLs aren't found.
// Solution offered here:
//   * discoverToolchain(): find the MinGW bin dir (g++ vs g++.exe / COLLECT_GCC).
//   * buildEnv(): returns an env object with the bin dir prepended to PATH (Windows-style).
//   * stageDlls(exePath): copy the needed runtime DLLs next to the exe (loader searches exe dir first).
// Exit 0/truthy = ready; only used by gates, never run directly.
import { spawnSync } from 'child_process';
import { copyFileSync, existsSync } from 'fs';
import { dirname, join } from 'path';

export const NEEDED_DLLS = ['libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libgcc_s_dw2-1.dll', 'libwinpthread-1.dll'];

export function discoverToolchain() {
  if (process.platform !== 'win32') return null;
  const probes = process.platform === 'win32' ? ['g++.exe', 'g++'] : ['g++'];
  for (const name of probes) {
    // 1) via WHERE.EXE (gives Windows-style absolute path)
    const w = spawnSync('where', [name], { encoding: 'utf8' });
    if (w.status === 0 && w.stdout) {
      const line = w.stdout.trim().split(/\r?\n/)[0];
      if (line) return dirname(line);
    }
    // 2) via bash `which` (gives /c/... form)
    const o = spawnSync('bash', ['-lc', `command -v ${name} 2>/dev/null`], { encoding: 'utf8' });
    if (o.status === 0 && o.stdout) {
      const p = o.stdout.trim();
      if (p && existsSync(p)) {
        // convert msys/cygwin path to Windows-style when possible
        const cy = spawnSync('cygpath', ['-w', p], { encoding: 'utf8' });
        if (cy.status === 0 && cy.stdout) return dirname(cy.stdout.trim());
        return dirname(p);
      }
    }
  }
  return null;
}

export function buildEnv() {
  const env = { ...process.env };
  const bin = discoverToolchain();
  if (bin && env.PATH) env.PATH = `${bin};${env.PATH}`;
  return env;
}

export function stageDlls(exePath) {
  if (process.platform !== 'win32') return;
  const bin = discoverToolchain();
  if (!bin) return;
  const outDir = dirname(exePath);
  let staged = 0;
  for (const dll of NEEDED_DLLS) {
    const src = join(bin, dll);
    if (existsSync(src)) {
      try { copyFileSync(src, join(outDir, dll)); staged++; } catch (e) { /* ignore */ }
    }
  }
  return staged;
}

// Allow being called as a standalone probe for CI diagnostics.
if (import.meta.url === `file:///${process.argv[1]?.replace(/\\/g, '/')}`) {
  const bin = discoverToolchain();
  console.log(`mingw-toolchain: ${bin || 'NOT FOUND on non-Windows or missing'}`);
}