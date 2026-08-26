#!/usr/bin/env node
// sbom-gen.mjs — AGENT-6 §5: generates a dependency SBOM from REAL usage
// (solutions referenced by the build) with reproducible version info, not
// from the mere presence of clones. Output: manifests/SBOM.json +
// manifests/SBOM.md (short human table).
//
//   node tools/portability/sbom-gen.mjs [--out manifests/SBOM.json]
import { readdirSync, readFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const ROOT = process.cwd();
const SOLUTIONS = join(ROOT, 'external', 'solutions');

const cmakeText = (() => {
  let t = '';
  try { t += readFileSync(join(ROOT, 'CMakeLists.txt'), 'utf8'); } catch {}
  const cmakeDir = join(ROOT, 'cmake');
  if (existsSync(cmakeDir)) {
    const stack = [cmakeDir];
    while (stack.length) {
      const dir = stack.pop();
      let es; try { es = readdirSync(dir, { withFileTypes: true }); } catch { continue; }
      for (const e of es) {
        const p = join(dir, e.name);
        if (e.isDirectory()) stack.push(p);
        else if (/\.(cmake|txt)$/.test(e.name)) { try { t += readFileSync(p, 'utf8'); } catch {} }
      }
    }
  }
  return t;
})();

function gitVersion(dir) {
  const res = spawnSync('git', ['-C', dir, 'describe', '--tags', '--always', '--dirty'], { encoding: 'utf8', timeout: 5000, windowsHide: true });
  if (res.status === 0) return res.stdout.trim();
  const head = spawnSync('git', ['-C', dir, 'rev-parse', '--short', 'HEAD'], { encoding: 'utf8', timeout: 5000, windowsHide: true });
  return head.status === 0 ? head.stdout.trim() : 'unknown';
}

const solutions = readdirSync(SOLUTIONS, { withFileTypes: true })
  .filter((e) => e.isDirectory())
  .map((e) => e.name)
  .sort();

const entries = [];
for (const name of solutions) {
  const lower = name.toLowerCase();
  // Real usage = referenced by the build graph (CMakeLists/cmake/), case-insensitive.
  const used = new RegExp(`(?:${lower}|solutions/${name})`, 'i').test(cmakeText);
  if (!used) continue;
  const dir = join(SOLUTIONS, name);
  entries.push({
    name,
    kind: 'external/solutions',
    version: gitVersion(dir),
    role: /target_link_libraries|add_library|add_executable|add_subdirectory/i.test(
      (cmakeText.match(new RegExp(`[^\\n]{0,80}${lower}[^\\n]{0,80}`, 'i')) || [''])[0]) ? 'linked' : 'referenced',
    license: name === 'zstd' ? 'BSD-3-Clause' : name === 'blake3' ? 'CC0-1.0' : 'see vendored LICENSE',
  });
}

// third_party (promoted deps actually compiled).
const thirdParty = [];
if (existsSync(join(ROOT, 'third_party'))) {
  for (const e of readdirSync(join(ROOT, 'third_party'), { withFileTypes: true })) {
    if (!e.isDirectory()) continue;
    const dir = join(ROOT, 'third_party', e.name);
    const hasSrc = readdirSync(dir).some((f) => /\.(cpp|hpp|h|cc|c)$/.test(f) || existsSync(join(dir, 'CMakeLists.txt')));
    if (hasSrc) thirdParty.push({ name: e.name, kind: 'third_party', version: gitVersion(dir), role: 'promoted' });
  }
}

const sbom = {
  engine: 'VulkanCraft',
  generated: new Date().toISOString(),
  source: 'real build-graph references (CMakeLists.txt + cmake/)',
  solutions_used: entries.length,
  third_party_promoted: thirdParty.length,
  dependencies: [...entries, ...thirdParty],
};

mkdirSync(join(ROOT, 'manifests'), { recursive: true });
const outPath = process.argv.includes('--out')
  ? process.argv[process.argv.indexOf('--out') + 1]
  : join('manifests', 'SBOM.json');
writeFileSync(join(ROOT, outPath), JSON.stringify(sbom, null, 2));

const md = `# SBOM — dependências efetivamente usadas (gerado: ${sbom.generated.slice(0, 10)})\n\n| solução | kind | versão | papel |\n|---|---|---|---|\n` +
  sbom.dependencies.map((d) => `| ${d.name} | ${d.kind} | ${d.version} | ${d.role} |`).join('\n') + '\n';
writeFileSync(join(ROOT, 'manifests', 'SBOM.md'), md);
console.log(`[sbom-gen] wrote ${outPath} — ${entries.length} solutions used + ${thirdParty.length} third_party promoted.`);
