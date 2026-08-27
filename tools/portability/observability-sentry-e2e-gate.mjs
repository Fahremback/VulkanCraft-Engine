#!/usr/bin/env node
// observability-sentry-e2e-gate.mjs — §6 item 6 E2E gate: compila o ADAPTER
// do contrato IObservability (src/engine/sdk/Observability.cpp) + o
// SentrySink (binding real sobre o sentry-native vendido, #306) contra a lib
// estática MSVC e roda o fluxo completo (sink substituível → eventos sentry
// reais com transport custom sem rede, opt-in respeitado, crash context
// preservado, persistência bit-exact). Exit 0 = o sink do contrato funciona
// com o backend real. Mesmo padrão do luau-sandbox-e2e-gate (#304).
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const SENTRY = join(ROOT, 'external', 'solutions', 'sentry-native');
const E2E = join(ROOT, 'tools', 'portability', 'observability-sentry-e2e.cpp');
const EXE = join(ROOT, 'build', 'Release', 'observability-sentry-e2e.exe');

const SENTRY_LIB = join(SENTRY, 'build-gate', 'Release', 'sentry.lib');
if (!existsSync(SENTRY_LIB)) {
  const b = spawnSync(join(SENTRY, 'build-sentry-gate.bat'), { encoding: 'utf8', shell: true, cwd: SENTRY });
  if (b.status !== 0) {
    console.error('[observability-sentry-e2e-gate] FAIL: sentry lib build error');
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
  `cl /nologo /EHsc /std:c++17 /O1 /MD /DSENTRY_BUILD_STATIC /I ${w(join(ROOT, 'src', 'engine', 'public'))} /I ${w(join(ROOT, 'src'))} /I ${w(join(SENTRY, 'include'))} ${w(E2E)} ${w(SENTRY_LIB)} user32.lib gdi32.lib dbghelp.lib winhttp.lib ws2_32.lib advapi32.lib bcrypt.lib shlwapi.lib crypt32.lib ole32.lib rpcrt4.lib version.lib ${w(join(SENTRY, 'kernelbase_extra.lib'))} kernel32.lib /Fe:${w(EXE)}`,
  'if errorlevel 1 exit /b 1',
  w(EXE)
].join('\r\n');
const batPath = join(ROOT, 'build', 'observability-sentry-e2e.bat');
const { writeFileSync } = await import('fs');
writeFileSync(batPath, bat, 'utf8');

const run = spawnSync('cmd.exe', ['/c', batPath], { encoding: 'utf8' });
if (run.status !== 0) {
  console.error('[observability-sentry-e2e-gate] FAIL');
  console.error(run.stdout + run.stderr);
  process.exit(1);
}
process.stdout.write(run.stdout);
console.log('[observability-sentry-e2e-gate] PASS — IObservability sink wired to real sentry-native');
process.exit(0);
