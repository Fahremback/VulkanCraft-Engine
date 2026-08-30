#!/usr/bin/env node
// luau-sandbox-e2e-gate.mjs — §3 item 6 E2E gate: compila o ADAPTER do
// contrato ILuauSandbox (src/engine/sdk/LuauSandbox.cpp) + o LuauRunner
// (binding real sobre o Luau vendido, #302) contra as libs estáticas MSVC do
// external/solutions/luau e roda o fluxo completo (attach runner real →
// evaluate de script Luau real → valor JSON real → erro runtime com tag →
// sandbox por construção → persistência bit-exact). Exit 0 = a política do
// contrato funciona com o backend real. Mesmo padrão do
// package-manager-sodium-gate (#299).
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
import { resolveExe } from './exe-resolve.mjs';
const LUAU = join(ROOT, 'external', 'solutions', 'luau');
const E2E = join(ROOT, 'tools', 'portability', 'luau-sandbox-e2e.cpp');
const EXE = resolveExe(ROOT, 'luau-sandbox-e2e');

const VM_LIB = join(LUAU, 'build-gate', 'Release', 'Luau.VM.lib');
if (!existsSync(VM_LIB)) {
  const b = spawnSync(join(LUAU, 'build-luau-libs.bat'), { encoding: 'utf8', shell: true, cwd: LUAU });
  if (b.status !== 0) {
    console.error('[luau-sandbox-e2e-gate] FAIL: lib build error');
    console.error(b.stdout + b.stderr);
    process.exit(1);
  }
}

const vcvars = 'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat';
const w = (p) => p.replace(/\//g, '\\');
const bat = [
  '@echo off',
  `call "${vcvars}" >nul 2>&1`,
  `cd /d ${w(ROOT)}`,
  `cl /nologo /EHsc /std:c++17 /O1 /MD /I ${w(join(ROOT, 'src', 'engine', 'public'))} /I ${w(join(ROOT, 'src'))} /I ${w(join(LUAU, 'VM', 'include'))} /I ${w(join(LUAU, 'Compiler', 'include'))} ${w(E2E)} ${w(join(LUAU, 'build-gate', 'Release', 'Luau.VM.lib'))} ${w(join(LUAU, 'build-gate', 'Release', 'Luau.Compiler.lib'))} ${w(join(LUAU, 'build-gate', 'Release', 'Luau.Ast.lib'))} ${w(join(LUAU, 'build-gate', 'Release', 'Luau.Bytecode.lib'))} ${w(join(LUAU, 'build-gate', 'Release', 'Luau.Common.lib'))} /Fe:${w(EXE)}`,
  'if errorlevel 1 exit /b 1',
  w(EXE)
].join('\r\n');
const batPath = join(ROOT, process.env.VC_BUILD_DIR || 'out/dev-shared', 'luau-sandbox-e2e.bat');
const { writeFileSync } = await import('fs');
writeFileSync(batPath, bat, 'utf8');

const run = spawnSync('cmd.exe', ['/c', batPath], { encoding: 'utf8' });
if (run.status !== 0) {
  console.error('[luau-sandbox-e2e-gate] FAIL');
  console.error(run.stdout + run.stderr);
  process.exit(1);
}
process.stdout.write(run.stdout);
console.log('[luau-sandbox-e2e-gate] PASS — ILuauSandbox policy wired to the real vendored Luau');
process.exit(0);
