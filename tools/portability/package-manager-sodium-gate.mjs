#!/usr/bin/env node
// package-manager-sodium-gate.mjs — gate E2E do wiring REAL entre o contrato
// IPackageManager (§6 item 5) e o libsodium (ed25519): compila o gate C++
// (package-manager-sodium-gate.cpp) contra o adapter real
// (src/engine/sdk/PackageManager.cpp) + sodium-verifier.hpp + a lib estática
// MSVC, e roda o fluxo completo (verify/install/tamper/wrong-key). Exit 0 =
// wiring real funcionando. Step 7f6b no ci-matrix.
import { spawnSync } from 'child_process';
import { existsSync, writeFileSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const SODIUM = join(ROOT, 'external', 'solutions', 'libsodium');
const INCLUDE_SODIUM = join(SODIUM, 'src', 'libsodium', 'include');
const LIB = join(SODIUM, 'bin', 'x64', 'Release', 'v143', 'static', 'libsodium.lib');
const EXE = join(ROOT, 'build', 'Release', 'package-manager-sodium-gate.exe');

if (!existsSync(LIB)) {
  console.error('[package-manager-sodium-gate] FAIL: libsodium lib missing — run libsodium-gate first');
  process.exit(1);
}

const vcvars = 'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat';
const w = (p) => p.replace(/\//g, '\\');
const bat = [
  '@echo off',
  `call "${vcvars}" >nul 2>&1`,
  `cd /d ${w(ROOT)}`,
  `cl /nologo /EHsc /std:c++17 /O1 /DSODIUM_STATIC /I ${w(INCLUDE_SODIUM)} /I ${w(join(ROOT, 'src', 'engine', 'public'))} /I ${w(join(ROOT, 'src'))} /I ${w(join(ROOT, 'tools', 'portability'))} ${w(join(ROOT, 'tools', 'portability', 'package-manager-sodium-gate.cpp'))} ${w(join(ROOT, 'src', 'engine', 'sdk', 'PackageManager.cpp'))} ${w(LIB)} /Fe:${w(EXE)}`,
  'if errorlevel 1 exit /b 1',
  w(EXE)
].join('\r\n');
const batPath = join(ROOT, 'tools', 'portability', 'build-package-manager-sodium.bat');
writeFileSync(batPath, bat, 'utf8');

const run = spawnSync('cmd.exe', ['/c', batPath], { encoding: 'utf8' });
if (run.status !== 0) {
  console.error('[package-manager-sodium-gate] FAIL');
  console.error(run.stdout + run.stderr);
  process.exit(1);
}
process.stdout.write(run.stdout);
console.log('[package-manager-sodium-gate] PASS — IPackageManager + libsodium ed25519 wiring real');
process.exit(0);
