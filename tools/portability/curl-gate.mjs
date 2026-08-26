#!/usr/bin/env node
// curl-gate.mjs — §7 gate de utilização do curl vendido (finding #300).
// Compila e roda tools/portability/curl-probe.c contra a lib estática MSVC
// (external/solutions/curl/build-gate/lib/Release/libcurl.lib). Exit 0 =
// utilizável. Mesmo padrão do immer/sqlite/libsodium gates.
import { spawnSync } from 'child_process';
import { existsSync, writeFileSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const CURL = join(ROOT, 'external', 'solutions', 'curl');
const INCLUDE = join(CURL, 'include');
const PROBE = join(ROOT, 'tools', 'portability', 'curl-probe.c');
const LIB = join(CURL, 'build-gate', 'lib', 'Release', 'libcurl.lib');
const EXE = join(ROOT, 'build', 'Release', 'curl-probe.exe');

if (!existsSync(LIB)) {
  console.error('[curl-gate] FAIL: static lib missing — build it with:');
  console.error('  cmd /c external/solutions/curl/build-curl-gate.bat');
  process.exit(1);
}

const vcvars = 'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat';
const w = (p) => p.replace(/\//g, '\\');
const bat = [
  '@echo off',
  `call "${vcvars}" >nul 2>&1`,
  `cd /d ${w(ROOT)}`,
  `cl /nologo /EHsc /O1 /MD /DCURL_STATICLIB /I ${w(INCLUDE)} ${w(PROBE)} ${w(LIB)} ws2_32.lib crypt32.lib advapi32.lib bcrypt.lib user32.lib secur32.lib shlwapi.lib iphlpapi.lib /Fe:${w(EXE)}`,
  'if errorlevel 1 exit /b 1',
  w(EXE)
].join('\r\n');
const batPath = join(CURL, 'build-curl-probe.bat');
writeFileSync(batPath, bat, 'utf8');

const run = spawnSync('cmd.exe', ['/c', batPath], { encoding: 'utf8' });
if (run.status !== 0) {
  console.error('[curl-gate] FAIL');
  console.error(run.stdout + run.stderr);
  process.exit(1);
}
process.stdout.write(run.stdout);
console.log('[curl-gate] PASS — vendored curl usable (URL API + escape + error gate)');
process.exit(0);
