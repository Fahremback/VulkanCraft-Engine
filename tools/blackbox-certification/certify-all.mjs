#!/usr/bin/env node
// certify-all.mjs — AGENT-6 §12: single command that installs the engine to an
// independent prefix, runs the full black-box certification, generates SBOM +
// hashes of the certified package, collects evidence, and returns non-zero on
// ANY incomplete requirement.
//
//   node tools/blackbox-certification/certify-all.mjs [--prefix <dir>] [--keep]
//
// Exit 0 = certified. Exit 1 = any requirement failed.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync, statSync, rmSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const ROOT = process.cwd();
const args = process.argv.slice(2);
const keep = args.includes('--keep');
const prefixArg = args.indexOf('--prefix');
const PREFIX = prefixArg >= 0 ? resolve(args[prefixArg + 1]) : join(tmpdir(), `vc-blackbox-${Date.now()}`);
const EVIDENCE = join(ROOT, 'out', 'artifacts', 'blackbox-certification');
const failures = [];

function log(m) { console.log(`[certify] ${m}`); }
function run(cmd, argsList, opts = {}) {
  const res = spawnSync(cmd, argsList, { cwd: ROOT, encoding: 'utf8', timeout: 900000, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'], ...opts });
  return res;
}
function hashFile(p) {
  const h = createHash('sha256');
  h.update(readFileSync(p));
  return h.digest('hex');
}

log(`prefix: ${PREFIX}`);
mkdirSync(EVIDENCE, { recursive: true });

// 1. Install the engine package to the independent prefix. Reconfigure first
//    so the install manifest reflects the CURRENT CMakeLists (the consumer
//    gate does the same; a stale tree misses libs silently).
if (!existsSync(join(PREFIX, 'lib')) && !existsSync(join(PREFIX, 'include'))) {
  log('reconfiguring main build…');
  const cfg = run('cmake', ['-S', '.', '-B', 'build']);
  if (cfg.status !== 0) failures.push(`reconfigure failed (${cfg.status})`);
  log('installing package (cmake --install build --prefix)…');
  const inst = run('cmake', ['--install', 'build', '--config', 'Release', '--prefix', PREFIX]);
  if (inst.status !== 0) {
    failures.push(`cmake --install failed (${inst.status})`);
    console.error((inst.stderr || '').slice(-800));
  } else log('install OK');
} else log('prefix already populated — reusing');

// 2. Run the hostile certification on the installed prefix.
log('running hostile certification…');
const cert = run('node', ['tools/blackbox-certification/run-certification.mjs', PREFIX]);
if (cert.status !== 0) failures.push('hostile certification failed (see run-certification output)');
else log('hostile certification PASSED');

// 3. SBOM + hashes of the certified package.
log('generating SBOM + hashes…');
const sbom = { engine: 'VulkanCraft', prefix: PREFIX, generated: new Date().toISOString(), files: [], hashes: {} };
function walk(dir, rel = '') {
  let es; try { es = readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const e of es) {
    const p = join(dir, e.name);
    const r = rel ? `${rel}/${e.name}` : e.name;
    if (e.isDirectory()) { walk(p, r); continue; }
    const size = statSync(p).size;
    if (size > 50 * 1024 * 1024) { sbom.files.push({ path: r, size, note: 'skipped hash (large binary)' }); continue; }
    sbom.files.push({ path: r, size });
    sbom.hashes[r] = hashFile(p);
  }
}
walk(PREFIX);
writeFileSync(join(EVIDENCE, 'certified-sbom.json'), JSON.stringify(sbom, null, 2));
writeFileSync(join(EVIDENCE, 'certified-prefix.txt'), `${PREFIX}\n`);

// 4. Summary evidence.
const summary = {
  engine: 'VulkanCraft',
  generated: new Date().toISOString(),
  prefix: PREFIX,
  installed: existsSync(join(PREFIX, 'include')) || existsSync(join(PREFIX, 'lib')),
  hostile_certification: cert.status === 0,
  files_hashed: Object.keys(sbom.hashes).length,
  failures,
};
writeFileSync(join(EVIDENCE, 'summary.json'), JSON.stringify(summary, null, 2));
log(`evidence: ${EVIDENCE}`);
console.log(`[certify] summary: ${JSON.stringify({ installed: summary.installed, hostile: summary.hostile_certification, files_hashed: summary.files_hashed, failures: failures.length })}`);

if (!keep) rmSync(PREFIX, { recursive: true, force: true });

if (failures.length) { console.error(`[certify] CERTIFICATION FAILED (${failures.length}):`); failures.forEach((f) => console.error(`  - ${f}`)); process.exit(1); }
console.log('[certify] CERTIFICATION PASSED — package installed, certified, SBOM+hashes collected.');
