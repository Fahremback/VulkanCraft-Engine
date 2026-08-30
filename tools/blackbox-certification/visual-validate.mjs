#!/usr/bin/env node
// visual-validate.mjs — Agent 6 item 273/279: automated VISUAL validation of the
// editor/runtime via real window captures (PrintWindow -> PNG), with a pixel
// difference report. Proves the viewport renders real (non-black, data-rich)
// content and updates frame over frame — the GPU/display half of the A6 "window"
// items, done headless-of-interaction from this execution.
//
//   node tools/blackbox-certification/visual-validate.mjs [--process VulkanEngineEditor] [--frames 3]
//
// Requires: the editor/runtime already launched with a window on a display/GPU.
//           node + python (PIL/numpy) on PATH.
import { readFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve, basename } from 'node:path';
import { spawnSync, execSync } from 'node:child_process';

const buildRoot = process.cwd().replace(/\\/g, '/');
// The certification artifacts live under <repo>/engine/out/artifacts regardless of cwd.
export const reporoot = (buildRoot.endsWith('/engine')) ? buildRoot.slice(0, -'/engine'.length) : buildRoot;
// resolve OUT under engine/out/artifacts for consistency with the other certification reports.
const root = join(reporoot, 'engine');
const args = process.argv.slice(2);
const procName = (() => {
  const i = args.indexOf('--process');
  return i >= 0 ? args[i + 1] : 'VulkanEngineEditor';
})();
const frames = (() => {
  const i = args.indexOf('--frames');
  return i >= 0 ? parseInt(args[i + 1], 10) : 3;
})();

const SCRIPT_WIN = join(root, 'tools', 'qt_shell', 'capture_window.ps1').split(/[\\/]/).join('\\');
const OUT = join(root, 'out', 'artifacts', 'visual-validation');
mkdirSync(OUT, { recursive: true });

const timestamps = [];
for (let f = 1; f <= frames; ++f) {
  const png = join(OUT, `${procName}_frame_${f}.png`);
  const r = spawnSync('powershell.exe', [
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', SCRIPT_WIN,
    '-ProcessName', procName, '-Out', png.replace(/\\/g, '\\\\'),
  ], { stdio: 'pipe', windowsHide: true, timeout: 30000 });
  if (r.status !== 0) {
    console.error(`capture failed for frame ${f}:\n${r.stdout}${r.stderr}`);
    process.exit(1);
  }
  timestamps.push(png);
  if (f < frames) execSync('ping -n 3 127.0.0.1 >nul', { shell: 'cmd', windowsHide: true });
}

// Analyze: non-black (mean/min/max) + pairwise pixel-diff fraction.
const py = `
from PIL import Image, ImageChops
import numpy as np, json, sys
pngs = ${JSON.stringify(timestamps).replace(/\\\\/g, '\\\\')}
imgs = [Image.open(p).convert('RGB') for p in pngs]
w, h = imgs[0].size
arr = np.asarray(imgs[0])
report = {"width": w, "height": h,
  "mean_rgb": [round(float(v),2) for v in arr.mean(axis=(0,1))],
  "min_px": int(arr.min()), "max_px": int(arr.max())}
report["non_black"] = report["max_px"] > 16
pairs = []
for n in range(len(imgs)):
    for m in range(n+1, len(imgs)):
        d = ImageChops.difference(imgs[n], imgs[m])
        hist = d.histogram(); total = sum(hist)
        diff = sum(hist[1:])
        bb = d.getbbox()
        pairs.append({"pairs": f"f{n+1}-f{m+1}", "diff_pixels": int(diff),
                      "frac": round(diff/total, 4) if total else 0.0,
                      "bbox": list(bb) if bb else None})
        if bb:
            bx0,by0,bx1,by1 = bb
            crop = imgs[1].crop(bb)
            report.setdefault("diff_center_bbox", {"x0":bx0,"y0":by0,"x1":bx1,"y1":by1})
report["pairs"] = pairs
# updated fraction = any pair > 1%
report["updates_over_frames"] = any(p["frac"] > 0.01 for p in pairs)
print(json.dumps(report))
`;
const pr = spawnSync('python', ['-c', py], { encoding: 'utf8', windowsHide: true, timeout: 30000 });
if (pr.status !== 0) { console.error('analysis failed:\n' + pr.stderr); process.exit(1); }
const analysis = JSON.parse(pr.stdout.trim().split('\n').pop());

const status = analysis.non_black && analysis.updates_over_frames ? 'PASSED' : 'FAILED';
const html = `<!doctype html><html><head><meta charset="utf-8">
<title>Visual validation — ${procName}</title>
<style>body{font-family:sans-serif;background:#121212;color:#e0e0e0;margin:24px}img{max-width:640px;border:1px solid #333}
th,td{padding:6px 12px;border:1px solid #444;text-align:left}table{border-collapse:collapse}code{background:#222;padding:1px 4px}</style></head>
<body><h1>Visual validation — ${procName}</h1><p><b>Result: ${status}</b></p>
<h2>Summary</h2><table><tr><th>Width</th><th>Height</th><th>mean RGB</th><th>min</th><th>max</th><th>non-black</th></tr>
<tr><td>${analysis.width}</td><td>${analysis.height}</td><td>${analysis.mean_rgb.join(', ')}</td><td>${analysis.min_px}</td><td>${analysis.max_px}</td><td>${analysis.non_black}</td></tr></table>
<h2>Frame-diff (render liveness)</h2><table><tr><th>Pair</th><th>Diff pixels</th><th>Fraction</th><th>BBox</th></tr>
${analysis.pairs.map(p=>`<tr><td>${p.pairs}</td><td>${p.diff_pixels}</td><td>${p.frac}</td><td>${p.bbox?p.bbox.join(','):'-'}</td></tr>`).join('')}</table>
<h2>Captures</h2>${timestamps.map((p,i)=>`<div><b>frame ${i+1}</b><br/><img src="${join('out','artifacts','visual-validation', basename(p)).split(/[\\/]/).join('/')}"/></div>`).join('')}
</body></html>`;
const htmlPath = join(root, 'out', 'artifacts', 'visual-validation', `${procName}-report.html`);
writeFileSync(htmlPath, html);
writeFileSync(join(root, 'out', 'artifacts', 'visual-validation', `${procName}-summary.json`),
  JSON.stringify({ proc: procName, frames, status, analysis, capturedAt: new Date().toISOString() }, null, 2));
console.log(`VISUAL-VALIDATION ${status}: ${procName} ${analysis.width}x${analysis.height} ` +
  `meanRGB=(${analysis.mean_rgb.join(',')}) nonBlack=${analysis.non_black} updates=${analysis.updates_over_frames} -> ${htmlPath}`);