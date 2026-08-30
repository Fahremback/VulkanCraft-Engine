#!/usr/bin/env node
// libsodium-gate.mjs — §7 gate de utilização do libsodium vendido (finding
// #298). Compila e roda tools/portability/libsodium-probe.cpp contra a lib
// estática MSVC (external/solutions/libsodium/bin/x64/Release/v143/static/
// libsodium.lib) — ed25519 sign/verify, o backend criptográfico do contrato
// ISignatureVerifier (§6 item 5). Exit 0 = utilizável; 1 = não.
// Mesmo padrão do immer-gate (#288) / sqlite-gate (#297).
import { spawnSync } from 'child_process';
import { existsSync, writeFileSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
import { resolveExe } from './exe-resolve.mjs';
const SODIUM = join(ROOT, 'external', 'solutions', 'libsodium');
const INCLUDE = join(SODIUM, 'src', 'libsodium', 'include');
const PROBE = join(ROOT, 'tools', 'portability', 'libsodium-probe.cpp');
const LIB = join(SODIUM, 'bin', 'x64', 'Release', 'v143', 'static', 'libsodium.lib');
const EXE = resolveExe(ROOT, 'libsodium-probe');

if (!existsSync(LIB)) {
  console.error('[libsodium-gate] FAIL: static lib missing — build it with the MSVC solution:');
  console.error('  cmd /c external/solutions/libsodium/builds/msvc/vs2019/build-sodium.bat');
  process.exit(1);
}

const vcvars = 'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat';
const bat = [
  '@echo off',
  `call "${vcvars}" >nul 2>&1`,
  `cd /d ${ROOT.replace(/\//g, '\\')}`,
  `cl /nologo /EHsc /O1 /DSODIUM_STATIC /I ${INCLUDE.replace(/\//g, '\\')} ${PROBE.replace(/\//g, '\\')} ${LIB.replace(/\//g, '\\')} /Fe:${EXE.replace(/\//g, '\\')}`,
  'if errorlevel 1 exit /b 1',
  `${EXE.replace(/\//g, '\\')}`
].join('\r\n');
const batPath = join(ROOT, 'external', 'solutions', 'libsodium', 'builds', 'msvc', 'vs2019', 'build-sodium-probe.bat');
writeFileSync(batPath, bat, 'utf8');

const run = spawnSync('cmd.exe', ['/c', batPath], { encoding: 'utf8' });
if (run.status !== 0) {
  console.error('[libsodium-gate] FAIL');
  console.error(run.stdout + run.stderr);
  process.exit(1);
}
process.stdout.write(run.stdout);
console.log('[libsodium-gate] PASS — vendored libsodium usable (ed25519 sign/verify)');
process.exit(0);
