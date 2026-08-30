#!/usr/bin/env node
// visual-compare.mjs — A5 §G-86: versioned-baseline VISUAL regression gate.
//
// Compares a freshly captured window PNG against a versioned baseline stored
// under out/artifacts/visual-validation/baseline/<name>.png with a per-pixel
// tolerance, optional mask regions, and a mean-diff threshold. Emits a JSON
// report and exits nonzero when the capture drifts beyond tolerance — catching
// gross regressions (black frame, wrong scene, resolution collapse).
//
//   node tools/blackbox-certification/visual-compare.mjs \
//       --capture out/artifacts/visual-validation/VulkanEngineEditor_frame_1.png \
//       --name editor_showcase --tolerance 12 --mask "0,0,64,64"
//
// Mask format: comma-separated "x,y,w,h" rectangles (multiple allowed, repeat
// the flag). Baseline creation: --record captures the current PNG as the new
// versioned baseline (only after human/visual confirmation).
import { readFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const args = process.argv.slice(2);
const getArg = (name) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined; };

const capturePath = getArg('--capture');
const name = getArg('--name') || 'default';
const tolerance = parseFloat(getArg('--tolerance') || '12');
const meanThreshold = parseFloat(getArg('--mean-threshold') || '3');
const record = args.includes('--record');
const masks = [];
for (let i = args.indexOf('--mask'); i >= 0; i = args.indexOf('--mask', i + 1)) {
  const m = args[i + 1];
  if (m) masks.push(m.split(',').map(Number));
}

if (!capturePath) {
  console.error('[visual-compare] --capture <png> required');
  process.exit(2);
}
const root = process.cwd().replace(/\\/g, '/');
const baselineDir = join(root, 'out', 'artifacts', 'visual-validation', 'baseline');
mkdirSync(baselineDir, { recursive: true });
const baselinePath = join(baselineDir, `${name}.png`);

if (record) {
  if (!existsSync(capturePath)) { console.error(`[visual-compare] capture missing: ${capturePath}`); process.exit(1); }
  writeFileSync(baselinePath, readFileSync(capturePath));
  const meta = { name, recordedAt: new Date().toISOString(), tolerance, meanThreshold, source: capturePath };
  writeFileSync(join(baselineDir, `${name}.json`), JSON.stringify(meta, null, 2) + '\n');
  console.log(`[visual-compare] baseline recorded → ${baselinePath}`);
  process.exit(0);
}

if (!existsSync(baselinePath)) {
  console.error(`[visual-compare] no baseline for '${name}' — record one with --record first`);
  process.exit(2);
}

const py = `
from PIL import Image
import numpy as np, json, sys
base = np.asarray(Image.open(r"${baselinePath.replace(/\\/g, '\\\\\\\\')}").convert("RGB"), dtype=np.int16)
cur  = np.asarray(Image.open(r"${capturePath.replace(/\\/g, '\\\\\\\\')}").convert("RGB"), dtype=np.int16)
mask = np.ones_like(base, dtype=bool)
for (x, y, w, h) in ${JSON.stringify(masks)}:
    mask[y:y+h, x:x+w, :] = False
diff = np.abs(cur - base)
tol = ${tolerance}
viol = float((diff[mask] > tol).sum()) if mask.any() else 0.0
total = float(mask.sum() // 3)
frac = viol / total if total else 0.0
mean = float(diff[mask].mean()) if mask.any() else 0.0
out = {"width": int(cur.shape[1]), "height": int(cur.shape[0]),
       "masked_pixels": int(total), "over_tolerance": int(viol),
       "violation_fraction": round(frac, 6), "mean_diff": round(mean, 3),
       "max_diff": int(diff[mask].max()) if mask.any() else 0}
print(json.dumps(out))
`;
const r = spawnSync('python', ['-c', py], { encoding: 'utf8', timeout: 60000, windowsHide: true });
if (r.status !== 0) {
  console.error('[visual-compare] python compare failed:', r.stderr);
  process.exit(1);
}
const rep = JSON.parse(r.stdout.trim());
rep.name = name;
rep.baseline = baselinePath;
rep.passed = rep.violation_fraction < 0.02 && rep.mean_diff <= meanThreshold;
const OUT = join(root, 'out', 'artifacts', 'visual-validation');
writeFileSync(join(OUT, `${name}-compare.json`), JSON.stringify(rep, null, 2) + '\n');
console.log(`[visual-compare] ${rep.passed ? 'PASS' : 'FAIL'} ${name}: mean_diff=${rep.mean_diff} over_tolerance=${rep.over_tolerance}/${rep.masked_pixels} (${(rep.violation_fraction * 100).toFixed(2)}%)`);
if (!rep.passed) { console.error('[visual-compare] drift beyond tolerance — inspect capture vs baseline'); process.exit(1); }
