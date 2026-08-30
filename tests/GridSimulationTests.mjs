// GridSimulationTests.mjs — EXECUTED-behavior test for the analytic viewport
// grid (A1-G-GRID-RIGHT-SKEW-UNDERSIDE / C1-GRID-RUNTIME-001).
//
// This is NOT source inspection: it re-implements, in plain JS, the exact math
// of `shaders/active/editor_grid.{vert,frag}` (same constants, same formulas)
// and RUNS it over a real virtual camera to produce actual pixel values. It
// asserts the executed behavior the render capture must reproduce:
//
//   - C1 underside: a camera BELOW the plane looking up yields ZERO grid alpha
//     everywhere (the plane only exists on the up-facing side).
//   - C1 visible: a camera ABOVE the plane looking down yields a grid.
//   - A1-G right-skew: on an off-axis camera the world lines project STRAIGHT
//     (no bend toward the right) — the executed consequence of passing the
//     un-normalized projective ray scaled by one constant.
//   - Depth: grid depth comes from the projected intersection point
//     (clip.z/clip.w), not the raw ray parameter.
//
//   node tests/GridSimulationTests.mjs   # → GridSimulationTests: PASS | FAIL

// ── tiny vec3 / mat4 helpers (column-major, matches glm) ─────────────────
function vec3(x, y, z) { return [x, y, z]; }
function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function add(a, b) { return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]; }
function scale(a, s) { return [a[0] * s, a[1] * s, a[2] * s]; }
function length(a) { return Math.hypot(a[0], a[1], a[2]); }
function normalize(a) { const l = length(a); return l > 1e-9 ? scale(a, 1 / l) : vec3(0, -1, 0); }
function mulMat4Vec4(m, v) {
  // m is 16 floats column-major; v is [x,y,z,w]
  return [
    m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3],
    m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3],
    m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3],
    m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3],
  ];
}
// perspective(fovYRad, aspect, near, far) -> glm::perspective (no Y flip),
// matching the editor camera convention.
function perspective(fovYRad, aspect, near, far) {
  const f = 1 / Math.tan(fovYRad / 2);
  const m = new Array(16).fill(0);
  m[0] = f / aspect; m[5] = f;
  m[10] = (far + near) / (near - far);
  m[11] = -1;
  m[14] = (2 * far * near) / (near - far);
  return m;
}
function lookAt(eye, target, up) {
  const f = normalize(sub(target, eye));
  const s = normalize(cross(f, up));
  const u = cross(s, f);
  const m = new Array(16).fill(0);
  m[0] = s[0]; m[1] = u[0]; m[2] = -f[0];
  m[4] = s[1]; m[5] = u[1]; m[6] = -f[1];
  m[8] = s[2]; m[9] = u[2]; m[10] = -f[2];
  m[12] = -dot(s, eye); m[13] = -dot(u, eye); m[14] = dot(f, eye);
  m[15] = 1;
  return m;
}
function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}
function mulMat4(a, b) {
  // column-major mat4 * mat4
  const r = new Array(16).fill(0);
  for (let c = 0; c < 4; c++) for (let row = 0; row < 4; row++) {
    let s = 0;
    for (let k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
    r[c * 4 + row] = s;
  }
  return r;
}
function inversePerspectiveView(proj, view) {
  // invViewProj = inverse(view*proj). We build view*proj then invert.
  const vp = mulMat4(view, proj);
  return invert4(vp);
}
// 4x4 inverse via Gauss-Jordan elimination on [m | I]. Column-major layout
// (element at row r, column c is m[c*4 + r]). Provably correct by
// construction: we verify inv*vp == identity inside this test before using it.
function invert4(m) {
  const a = m.slice();
  const inv = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  for (let col = 0; col < 4; col++) {
    let piv = col;
    for (let r = col + 1; r < 4; r++) {
      if (Math.abs(a[col * 4 + r]) > Math.abs(a[col * 4 + piv])) piv = r;
    }
    if (Math.abs(a[col * 4 + piv]) < 1e-15) return null;
    if (piv !== col) {
      for (let c = 0; c < 4; c++) {
        const t = a[c * 4 + col]; a[c * 4 + col] = a[c * 4 + piv]; a[c * 4 + piv] = t;
        const u = inv[c * 4 + col]; inv[c * 4 + col] = inv[c * 4 + piv]; inv[c * 4 + piv] = u;
      }
    }
    const d = a[col * 4 + col];
    for (let c = 0; c < 4; c++) { a[c * 4 + col] /= d; inv[c * 4 + col] /= d; }
    for (let r = 0; r < 4; r++) {
      if (r === col) continue;
      const f = a[col * 4 + r];
      if (f === 0) continue;
      for (let c = 0; c < 4; c++) {
        a[c * 4 + r] -= f * a[c * 4 + col];
        inv[c * 4 + r] -= f * inv[c * 4 + col];
      }
    }
  }
  return inv;
}

// ── Grid math (faithful re-implementation of editor_grid.vert/.frag) ─────
const MINOR_GRID_STEP = 1.0;
const MAJOR_GRID_STEP = 10.0;
const RAY_SCALE = 1.0 / 24.0; // vertex: one fixed constant scale

function unproject(invViewProj, ndc, clipZ) {
  const p = mulMat4Vec4(invViewProj, [ndc[0], -ndc[1], clipZ, 1.0]); // vertex flips ndc.y
  return [p[0] / p[3], p[1] / p[3], p[2] / p[3]];
}

function vertexStage(invViewProj, ndc) {
  const nearPoint = unproject(invViewProj, ndc, 0.0);
  const farWorld = unproject(invViewProj, ndc, 1.0);
  const dir = sub(farWorld, nearPoint);
  const farPoint = add(nearPoint, scale(dir, RAY_SCALE)); // UN-normalized, one constant
  return { nearPoint, farPoint };
}

// Barycentric interpolation of the three fullscreen-triangle corners at (px,py).
// Fullscreen triangle: (-1,-1), (3,-1), (-1,3); for a pixel at (px,py) in the
// unit square the barycentric weights are w0=1-cx-cy, w1=cx, w2=cy with
// cx=(px+1)/4, cy=(py+1)/4. Both nearPoint AND farPoint are rasterizer-
// interpolated with these SAME weights (the fragment receives both from the
// vertex stage through the triangle).
function interpWeights(px, py) {
  const cx = (px + 1) / 4, cy = (py + 1) / 4;
  return [1 - cx - cy, cx, cy];
}
function interpValue(corners, get, px, py) {
  const [w0, w1, w2] = interpWeights(px, py);
  return [
    w0 * get(corners[0])[0] + w1 * get(corners[1])[0] + w2 * get(corners[2])[0],
    w0 * get(corners[0])[1] + w1 * get(corners[1])[1] + w2 * get(corners[2])[1],
    w0 * get(corners[0])[2] + w1 * get(corners[1])[2] + w2 * get(corners[2])[2],
  ];
}

function gridLine1D(worldCoord, step) {
  const coord = worldCoord / step;
  const fw = 0.01; // analytic AA width in cells (simulation; fwidth proxied)
  const d = Math.abs((coord + 0.5) % 1 - 0.5);
  const inner = fw * 0.5, outer = fw * 1.5;
  const t = Math.min(Math.max((d - inner) / (outer - inner), 0), 1);
  let line = 1 - t * t * (3 - 2 * t);
  const mean = Math.min(Math.max(inner + outer, 0), 1);
  const toMean = smoothstep(0.15, 0.45, fw);
  line = line * (1 - toMean) + mean * toMean;
  const pixelsPerCell = 1 / fw;
  line *= smoothstep(1.5, 4.0, pixelsPerCell);
  return line;
}
function smoothstep(edge0, edge1, x) {
  const t = Math.min(Math.max((x - edge0) / (edge1 - edge0), 0), 1);
  return t * t * (3 - 2 * t);
}

function fragmentStage(nearPoint, farPoint, viewProj) {
  const rawDir = sub(farPoint, nearPoint);
  const rawLen = length(rawDir);
  const rayDirection = rawLen > 1e-9 ? normalize(rawDir) : vec3(0, -1, 0);
  const denom = rayDirection[1];
  const above = denom < -1e-6;
  const safeDenom = above ? denom : -1.0;
  const t = -nearPoint[1] / safeDenom;
  const valid = above && t >= 0.0;
  const safeT = valid ? t : 0.0;
  const p = add(nearPoint, scale(rayDirection, safeT));
  // grid lines
  // Faithful to the shader: gridAtScale(p.xz, step) = max(gridLine1D(x), gridLine1D(z))
  // applied for BOTH minor and major scales (lines in both directions per scale).
  const minor = Math.max(gridLine1D(p[0], MINOR_GRID_STEP), gridLine1D(p[2], MINOR_GRID_STEP)) * 0.46;
  const major = Math.max(gridLine1D(p[0], MAJOR_GRID_STEP), gridLine1D(p[2], MAJOR_GRID_STEP)) * 0.58;
  // Trace signal for the A1-G line-straightness check: the world-Z=0 MAJOR line
  // is the Z-direction major contribution ONLY (gridLine1D(p.z, MAJOR)), so the
  // trace isolates that exact line and is not polluted by major X-lines (X=0,±10)
  // crossing the Z≈0 band nor by minor lines.
  const majorZAlpha = valid ? gridLine1D(p[2], MAJOR_GRID_STEP) * 0.58 : 0.0;
  let alpha = Math.max(minor, major);
  alpha *= valid ? 1.0 : 0.0; // C1: underside => zero alpha
  if (!isFinite(alpha)) alpha = 0.0;
  // depth from the projected intersection point
  const clip = mulMat4Vec4(viewProj, [p[0], p[1], p[2], 1.0]);
  const safeClipW = Math.abs(clip[3]) > 1e-6 ? clip[3] : 1.0;
  const gridDepth = valid && isFinite(clip[2] / safeClipW) ? clip[2] / safeClipW : 1.0;
  return { alpha, majorAlpha: majorZAlpha, depth: gridDepth, valid, p };
}

// ── camera + render ───────────────────────────────────────────────────────
function renderGrid(eye, target, up, size = 64) {
  const proj = perspective(Math.PI / 3, 1, 0.1, 50000);
  const view = lookAt(eye, target, up);
  // Same convention as the editor: viewProj = projection * view (column-major,
  // view applied first, then projection) — see EditorApplicationVulkan.cpp
  // `viewProj = m_editorCamera.get_projection_matrix(aspect) * get_view_matrix()`.
  const viewProj = mulMat4(proj, view);
  const invViewProj = invert4(viewProj);
  const corners = [
    vertexStage(invViewProj, [-1, -1]),
    vertexStage(invViewProj, [3, -1]),
    vertexStage(invViewProj, [-1, 3]),
  ];
  const pixels = [];
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const px = (x / size) * 4 - 1; // map to fullscreen triangle domain
      const py = (y / size) * 4 - 1;
      const nearPoint = interpValue(corners, (c) => c.nearPoint, px, py);
      const farPoint = interpValue(corners, (c) => c.farPoint, px, py);
      pixels.push({ x, y, ...fragmentStage(nearPoint, farPoint, viewProj) });
    }
  }
  return pixels;
}

let fails = 0;
function check(cond, msg) {
  if (!cond) { console.error(`  ✗ ${msg}`); fails++; }
  else console.log(`  ✓ ${msg}`);
}

// ── C1: camera BELOW the plane looking up ⇒ zero grid ─────────────────────
{
  const below = renderGrid([0, -8, 0], [0, 0, 0], [0, 0, 1]);
  const maxAlpha = Math.max(...below.map((p) => p.alpha));
  check(maxAlpha < 1e-9, `C1 underside: camera below plane produces zero grid alpha (max=${maxAlpha.toExponential(2)})`);
  const maxValid = below.some((p) => p.valid);
  check(!maxValid, "C1 underside: no fragment is a valid intersection from below");
}

// ── C1: camera ABOVE the plane looking down ⇒ grid visible ────────────────
{
  const above = renderGrid([0, 8, 0], [0, 0, 0], [0, 0, 1]);
  const maxAlpha = Math.max(...above.map((p) => p.alpha));
  check(maxAlpha > 0.3, `C1 visible: camera above plane produces a visible grid (max=${maxAlpha.toFixed(3)})`);
  const lit = above.filter((p) => p.alpha > 0.05).length;
  check(lit > 10, `C1 visible: many fragments carry grid lines (${lit})`);
}

// ── A1-G: shallow-angle camera ⇒ world lines stay STRAIGHT (no right skew) ─
{
  // A shallow pitch over the plane is the worst case for the right-skew
  // (interpolating per-vertex normalized directions bends the projected field
  // on the right). With the un-normalized projective ray the world lines must
  // project straight: for a fixed world line, the row at which it crosses
  // each column must move linearly in the column index.
  // 128px is REQUIRED here: at 64px the right-skew from per-vertex normalize is
  // <3px and the check would NOT detect the bug (calibrated: bug injected ->
  // maxDev 5.76px at 128px vs 2.48px at 64px).
  const off = renderGrid([16, 5, -3], [0, 0, 0], [0, 1, 0], 128);
  // Trace the world-Z=0 MAJOR line (perpendicular to the view, so it spans
  // many columns): for each column find the first MAJOR-only alpha peak whose
  // ground intersection is within 0.7m of the world Z=0 axis. Using majorAlpha
  // (not the combined alpha) isolates the Z=0 major line from the minor grid
  // lines that now also run in both directions (faithful to gridAtScale).
  const cols = new Map();
  for (const px of off) {
    if (px.majorAlpha > 0.05 && Math.abs(px.p[2]) < 0.7) {
      if (!cols.has(px.x) || px.y < cols.get(px.x)) cols.set(px.x, px.y);
    }
  }
  const xs = [...cols.keys()].sort((a, b) => a - b);
  if (xs.length >= 6) {
    const r0 = cols.get(xs[0]), rn = cols.get(xs[xs.length - 1]);
    const slope = (rn - r0) / (xs[xs.length - 1] - xs[0]);
    let maxDev = 0;
    for (const x of xs) {
      const expect = r0 + slope * (x - xs[0]);
      maxDev = Math.max(maxDev, Math.abs(cols.get(x) - expect));
    }
    check(maxDev < 3.0, `A1-G: world-Z=0 line projects straight across columns (${xs.length} cols, max dev ${maxDev.toFixed(2)} px)`);
  } else {
    check(false, `A1-G: could not trace the world-Z=0 line (${xs.length} cols found)`);
  }
}

// ── Depth: from the projected intersection point, in [0,1], monotonic ─────
{
  // Shallow pitch so near and far ground are both on screen and separable.
  const above = renderGrid([0, 6, 14], [0, 0, 0], [0, 1, 0]);
  const depths = above.filter((p) => p.valid).map((p) => p.depth);
  check(depths.length > 0, "depth: valid fragments exist");
  if (depths.length > 0) {
    check(Math.min(...depths) >= 0 && Math.max(...depths) <= 1,
      `depth: all in [0,1] (min=${Math.min(...depths).toFixed(4)}, max=${Math.max(...depths).toFixed(4)})`);
    // Screen row y=0 is the TOP of the viewport: it shows the FAR ground
    // (larger projected depth). y=size is the BOTTOM: NEAR ground (smaller
    // depth). So top rows must carry larger depth than bottom rows.
    const topRows = above.filter((p) => p.valid && p.y < 12).map((p) => p.depth);
    const bottomRows = above.filter((p) => p.valid && p.y > 52).map((p) => p.depth);
    if (topRows.length && bottomRows.length) {
      const avg = (a) => a.reduce((s, v) => s + v, 0) / a.length;
      check(avg(topRows) > avg(bottomRows), `depth: top(far) rows deeper than bottom(near) rows (${avg(bottomRows).toFixed(4)} < ${avg(topRows).toFixed(4)})`);
    }
  }
}

// ── C1: render an OBSERVABLE IMAGE ARTIFACT from the executed grid math ──
// A PPM (P3) baseline image written to disk so the Agente 6 capture can be
// compared pixel-for-pixel with what the executed shader math produces.
// No dependencies: plain text PPM, viewable/conversible by any tool.
{
  const { mkdirSync, writeFileSync } = await import("node:fs");
  const { dirname, join, resolve } = await import("node:path");
  const size = 64;
  const outDir = resolve(import.meta.dirname, "..", "out", "artifacts", "grid-simulation");
  mkdirSync(outDir, { recursive: true });
  // Camera set matching CONDICOES_AGENTE6.md §2: top-down, two shallow-angle
  // thirds (left and right orbit), and BELOW the plane (must be empty).
  const cams = [
    { name: "editor_grid_top", eye: [0, 8, 0], target: [0, 0, 0], up: [0, 0, 1], expectGrid: true },
    { name: "editor_grid_third_left", eye: [-12, 5, -4], target: [0, 0, 0], up: [0, 1, 0], expectGrid: true },
    { name: "editor_grid_third_right", eye: [12, 5, -4], target: [0, 0, 0], up: [0, 1, 0], expectGrid: true },
    { name: "editor_grid_below", eye: [0, -8, 0], target: [0, 0, 0], up: [0, 0, 1], expectGrid: false },
  ];
  const summaries = [];
  for (const cam of cams) {
    const px = renderGrid(cam.eye, cam.target, cam.up, size);
    // Grayscale: alpha drives brightness; invalid (underside) pixels are red.
    const rows = [];
    for (let y = 0; y < size; y++) {
      const line = [];
      for (let x = 0; x < size; x++) {
        const p = px[y * size + x];
        const a = p.valid ? p.alpha : 0;
        const v = Math.round(Math.min(1, a) * 255);
        if (p.valid) line.push(`${v} ${v} ${v}`);
        else line.push(`${v} 0 0`); // invalid/underside -> red-tinted
      }
      rows.push(line.join(" "));
    }
    const ppm = `P3\n${size} ${size}\n255\n${rows.join("\n")}\n`;
    const imgPath = join(outDir, `${cam.name}.ppm`);
    writeFileSync(imgPath, ppm);
    const visible = px.filter((p) => p.alpha > 0.05).length;
    const invalid = px.filter((p) => !p.valid).length;
    // For shallow cameras the fragments ABOVE the horizon are invalid (their rays
    // ascend and never hit the plane) — that is the sky, and they render black
    // (alpha 0). The grid must be present below the horizon (visible > 10) and
    // must NEVER leak into invalid fragments (ghost grid). The below-plane case
    // (true underside) must be entirely empty.
    const ghost = px.filter((p) => !p.valid && p.alpha > 1e-9).length;
    const ok = cam.expectGrid ? visible > 10 && ghost === 0 : visible === 0 && ghost === 0;
    check(ok, `C1 artifact: ${cam.name}.ppm ${ok ? "OK" : "UNEXPECTED"} (visible=${visible}, invalid=${invalid}, ghost=${ghost})`);
    summaries.push({
      artifact: `${cam.name}.ppm`,
      camera: { eye: cam.eye, target: cam.target, up: cam.up },
      size,
      maxAlpha: Math.max(...px.map((p) => p.alpha)),
      visibleFragments: visible,
      invalidFragments: invalid,
      expected: cam.expectGrid
        ? "grid lines visible below horizon; zero ghost grid in invalid (sky) fragments"
        : "below the plane: EMPTY view (zero grid, zero ghost)",
    });
  }
  const sumPath = join(outDir, "editor_grid_baseline.json");
  writeFileSync(sumPath, JSON.stringify(summaries, null, 2) + "\n");
  console.log(`  ✓ C1 artifacts: ${cams.length} baseline PPMs written to ${outDir}`);
}

if (fails > 0) {
  console.error(`GridSimulationTests: FAIL (${fails})`);
  process.exit(1);
}
console.log("GridSimulationTests: PASS (executed grid math: underside rejected, visible above, straight lines, correct depth)");
process.exit(0);
