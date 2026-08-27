#!/usr/bin/env node
// luau-gate.mjs — §7 gate de utilização do Luau vendido (finding #302).
// Builda as libs estáticas MSVC (Luau.VM/Compiler/Ast/Bytecode/Common via
// build-luau-libs.bat — configuração própria, nada de CMake da engine) e
// compila+roda tools/portability/luau-probe.cpp (compile→bytecode→VM→run→
// resultado, erro de runtime propagado, C API de tabelas/funções). Exit 0 =
// runtime de scripting vendido é utilizável SEM wiring. Mesmo padrão dos
// gates immer/sqlite/libsodium/curl/tuf.
import { spawnSync } from 'child_process';
import { existsSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const LUAU = join(ROOT, 'external', 'solutions', 'luau');
const PROBE_BAT = join(LUAU, 'build-luau-probe.bat');
const LIBS_BAT = join(LUAU, 'build-luau-libs.bat');

// Ensure the static libs exist (build once; incremental after that).
const VM_LIB = join(LUAU, 'build-gate', 'Release', 'Luau.VM.lib');
if (!existsSync(VM_LIB)) {
  const b = spawnSync(LIBS_BAT, { encoding: 'utf8', shell: true, cwd: LUAU });
  if (b.status !== 0) {
    console.error('[luau-gate] FAIL: lib build error');
    console.error(b.stdout + b.stderr);
    process.exit(1);
  }
}

const run = spawnSync(PROBE_BAT, { encoding: 'utf8', shell: true, cwd: ROOT });
process.stdout.write(run.stdout);
if (run.status !== 0) {
  console.error('[luau-gate] FAIL');
  process.stderr.write(run.stderr);
  process.exit(1);
}
console.log('[luau-gate] PASS — vendored Luau usable (compile + VM run + errors + C API)');
process.exit(0);
