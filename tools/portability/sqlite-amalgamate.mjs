#!/usr/bin/env node
// sqlite-amalgamate.mjs — gera o amalgamation do SQLite vendido de forma
// REPRODUTÍVEL a partir da árvore de fonte (external/solutions/sqlite),
// seguindo a cadeia canônica do Makefile.msc (finding #297).
//
// A árvore vendida NÃO traz sqlite3.c/sqlite3.h prontos (é source-form, 125+
// TUs); o amalgamation é o artefato oficial de consumo. Esta cadeia gera:
//  1. lemon (tool/lemon.c → lemon.exe)
//  2. mksourceid (tool/mksourceid.c → mksourceid.exe)
//  3. sqlite3.h (tool/mksqlite3h.tcl)
//  4. parse.c/parse.h (lemon sobre src/parse.y)
//  5. opcodes.h/opcodes.c (mkopcodeh/c.tcl sobre parse.h+vdbe.c)
//  6. keywordhash.h (mkkeywordhash.c → exe sobre parse.y)
//  7. pragma.h (mkpragmatab.tcl — escreve em cwd, NÃO em stdout)
//  8. ctime.c (mkctimec.tcl)
//  9. fts5parse.c/h (lemon sobre ext/fts5/fts5parse.y) + fts5.c (mkfts5c.tcl)
// 10. staging tsrc/ (cópia de src + ext + gerados)
// 11. sqlite3.c (mksqlite3c.tcl --srcdir tsrc)
// Exit 0 = amalgamation gerado; 1 = falhou (mensagem no stderr).
import { spawnSync } from 'child_process';
import { existsSync, mkdirSync, readdirSync, copyFileSync, rmSync, writeFileSync, readFileSync, statSync } from 'fs';
import { join } from 'path';

const ROOT = process.cwd();
const S = join(ROOT, 'external', 'solutions', 'sqlite');

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, {
    encoding: 'utf8',
    cwd: opts.cwd,
    input: opts.input,
    stdio: opts.silent ? ['pipe', 'pipe', 'pipe'] : 'inherit'
  });
  if (r.status !== 0 && !opts.allowFail) {
    console.error(`[sqlite-amalgamate] FAIL: ${cmd} ${args.join(' ')} (cwd=${opts.cwd || process.cwd()})`);
    if (opts.silent) console.error(r.stdout + r.stderr);
    process.exit(1);
  }
  return r;
}

function cpDir(srcDir, dstDir, exts) {
  if (!existsSync(srcDir)) return;
  for (const f of readdirSync(srcDir)) {
    if (exts.some((e) => f.endsWith(e))) {
      copyFileSync(join(srcDir, f), join(dstDir, f));
    }
  }
}

const cc = process.env.CC || 'gcc';

// 1. lemon + mksourceid (ferramentas canônicas).
if (!existsSync(join(S, 'lemon.exe'))) {
  run(cc, ['-O2', 'tool/lemon.c', '-o', 'lemon'], { cwd: S });
}
if (!existsSync(join(S, 'mksourceid.exe'))) {
  run(cc, ['-O2', 'tool/mksourceid.c', '-o', 'mksourceid'], { cwd: S });
}
if (!existsSync(join(S, 'mkkeywordhash.exe'))) {
  run(cc, ['-O2', 'tool/mkkeywordhash.c', '-o', 'mkkeywordhash'], { cwd: S });
}
copyFileSync(join(S, 'tool', 'lempar.c'), join(S, 'lempar.c'));

// 2. sqlite3.h (tcl gera em stdout → redireciona).
const hdr = run('tclsh', ['tool/mksqlite3h.tcl', '.'], { cwd: S, silent: true });
writeFileSync(join(S, 'sqlite3.h'), hdr.stdout, 'utf8');

// 3. parse.c/parse.h via lemon (lemon escreve ao lado do .y, em src/).
run(join(S, 'lemon.exe'), ['-Tlempar.c', 'src/parse.y'], { cwd: S, silent: true });
if (!existsSync(join(S, 'src', 'parse.c')) || !existsSync(join(S, 'src', 'parse.h'))) {
  console.error('[sqlite-amalgamate] FAIL: lemon did not produce src/parse.c|h');
  process.exit(1);
}

// 4. opcodes.h/opcodes.c (precisam de parse.h + vdbe.c no stdin).
{
  const src = readFileSync(join(S, 'src', 'parse.h'), 'utf8') +
              readFileSync(join(S, 'src', 'vdbe.c'), 'utf8');
  const oh = run('tclsh', ['tool/mkopcodeh.tcl'], { cwd: S, silent: true, input: src });
  writeFileSync(join(S, 'src', 'opcodes.h'), oh.stdout, 'utf8');
  const oc = run('tclsh', ['tool/mkopcodec.tcl', 'src/opcodes.h'], { cwd: S, silent: true });
  writeFileSync(join(S, 'src', 'opcodes.c'), oc.stdout, 'utf8');
}

// 5. keywordhash.h.
{
  const kh = run(join(S, 'mkkeywordhash.exe'), ['src/parse.y'], { cwd: S, silent: true });
  writeFileSync(join(S, 'src', 'keywordhash.h'), kh.stdout, 'utf8');
}

// 6. pragma.h — mkpragmatab.tcl escreve em cwd (pragma.h), NÃO em stdout.
run('tclsh', ['tool/mkpragmatab.tcl'], { cwd: S, silent: true });
if (!existsSync(join(S, 'pragma.h'))) {
  console.error('[sqlite-amalgamate] FAIL: pragma.h not generated');
  process.exit(1);
}

// 7. ctime.c.
{
  const ct = run('tclsh', ['tool/mkctimec.tcl'], { cwd: S, silent: true });
  if (!existsSync(join(S, 'ctime.c'))) {
    writeFileSync(join(S, 'ctime.c'), ct.stdout, 'utf8');
  }
}

// 8. fts5parse.c/h + fts5.c (lemon escreve ao lado do .y, em ext/fts5/).
run(join(S, 'lemon.exe'), ['-Tlempar.c', 'ext/fts5/fts5parse.y'], { cwd: S, silent: true });
if (!existsSync(join(S, 'ext', 'fts5', 'fts5parse.c')) ||
    !existsSync(join(S, 'ext', 'fts5', 'fts5parse.h'))) {
  console.error('[sqlite-amalgamate] FAIL: lemon did not produce ext/fts5/fts5parse.c|h');
  process.exit(1);
}
run('tclsh', ['tool/mkfts5c.tcl'], { cwd: join(S, 'ext', 'fts5'), silent: true });

// 9. staging tsrc/.
rmSync(join(S, 'tsrc'), { recursive: true, force: true });
mkdirSync(join(S, 'tsrc'), { recursive: true });
cpDir(join(S, 'src'), join(S, 'tsrc'), ['.c', '.h']);
for (const f of ['sqlite3.h', 'pragma.h', 'ctime.c']) {
  copyFileSync(join(S, f), join(S, 'tsrc', f));
}
// Gerados que nascem em src/ (lemon e os mk*.tcl):
for (const f of ['parse.c', 'parse.h', 'opcodes.h', 'opcodes.c', 'keywordhash.h']) {
  copyFileSync(join(S, 'src', f), join(S, 'tsrc', f));
}
// ext: os módulos que o amalgamation inclui (flist do mksqlite3c.tcl).
for (const sub of ['recover', 'rtree', 'session', 'icu', 'misc', 'rbu', 'fts3']) {
  cpDir(join(S, 'ext', sub), join(S, 'tsrc'), ['.c', '.h']);
}
copyFileSync(join(S, 'ext', 'fts5', 'fts5.c'), join(S, 'tsrc', 'fts5.c'));
copyFileSync(join(S, 'ext', 'fts5', 'fts5.h'), join(S, 'tsrc', 'fts5.h'));

// 10. sqlite3.c (amalgamation final).
run('tclsh', ['tool/mksqlite3c.tcl', '--srcdir', 'tsrc'], { cwd: S, silent: true });
if (!existsSync(join(S, 'sqlite3.c'))) {
  console.error('[sqlite-amalgamate] FAIL: sqlite3.c not generated');
  process.exit(1);
}
console.log(`[sqlite-amalgamate] OK — sqlite3.c (${Math.round(
  statSync(join(S, 'sqlite3.c')).size / 1024)} KB) + sqlite3.h`);
process.exit(0);
