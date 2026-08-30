import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const shaderPath = resolve(here, "../shaders/active/editor_grid.frag");
const shader = readFileSync(shaderPath, "utf8");
const vertPath = resolve(here, "../shaders/active/editor_grid.vert");
const vert = readFileSync(vertPath, "utf8");

assert.doesNotMatch(
  shader,
  /floor\s*\(\s*log\s*\(/,
  "viewport grid must not switch a visible scale through floor(log(...))"
);
assert.doesNotMatch(
  shader,
  /\bdecade\b/,
  "viewport grid color and spacing must not jump at logarithmic decade boundaries"
);
assert.match(shader, /const\s+float\s+MINOR_GRID_STEP\s*=\s*1\.0\s*;/);
assert.match(shader, /const\s+float\s+MAJOR_GRID_STEP\s*=\s*10\.0\s*;/);
assert.match(shader, /gridAtScale\s*\(\s*p\.xz\s*,\s*MINOR_GRID_STEP\s*\)/);
assert.match(shader, /gridAtScale\s*\(\s*p\.xz\s*,\s*MAJOR_GRID_STEP\s*\)/);
assert.match(
  shader,
  /float\s+worldPerPixel\s*=\s*max\s*\(\s*max\s*\(\s*length\s*\(\s*dFdx\s*\(\s*p\.xz\s*\)\s*\)\s*,\s*length\s*\(\s*dFdy\s*\(\s*p\.xz\s*\)\s*\)\s*\)/,
  "all line orientations must share the complete projected cell footprint"
);
assert.match(shader, /float\s+cellVisibility\s*\(\s*float\s+step\s*,\s*float\s+worldPerPixel\s*\)/);
assert.match(
  shader,
  /axisVisibility\s*=\s*cellVisibility\s*\(\s*MINOR_GRID_STEP\s*,\s*worldPerPixel\s*\)/,
  "world axes must retire with the grid instead of continuing alone to infinity"
);
assert.match(shader, /axisLine\s*\(\s*p\.z\s*,\s*1\.55\s*\)\s*\*\s*axisVisibility/);
assert.match(shader, /axisLine\s*\(\s*p\.x\s*,\s*1\.55\s*\)\s*\*\s*axisVisibility/);
assert.doesNotMatch(
  shader,
  /for\s*\([^)]*\)[\s\S]{0,240}pow\s*\(\s*10\.0/,
  "viewport grid must not replace its scale family according to camera distance"
);

// BUG-EDITOR-GRID-003: coplanar surfaces (terrain falloff ring at Y=0, entity
// bottom faces) tie the analytic grid depth to the rasterized mesh depth up
// to ~3 fp32 ULPs; without a deterministic advantage the LEQUAL outcome is a
// per-pixel coin flip that re-rolls on every camera rotation (the "dancing
// grid"). The fragment shader must keep the relative ULP nudge toward the
// camera, because the rasterizer depth bias never reaches a gl_FragDepth
// written by the shader.
assert.match(
  shader,
  /gridDepth\s*-=\s*gridDepth\s*\*\s*1\.2e-6\s*;/,
  "grid depth must keep the ~10 ULP coplanar tie-break nudge toward the camera"
);

// A1-G-GRID-RIGHT-SKEW-UNDERSIDE / C1-GRID-RUNTIME-001 (executable invariants
// that the Agente 6 capture must reproduce): the VERTEX stage must hand the
// rasterizer a PROJECTIVE, UN-normalized ray vector (scaled by one fixed
// constant), because per-vertex normalize() bends the direction field across
// the fullscreen triangle and skews the plane on the right of the camera. The
// FRAGMENT stage performs the ONE normalization after interpolation and must
// reject the underside (ray descending onto Y=0, i.e. denom < -1e-6) outright.
// These regex assertions are the local-executable stand-in for the render
// capture; they FAIL if someone re-introduces per-vertex normalize / drops the
// underside rejection.
assert.match(
  vert,
  /vec3\s+dir\s*=\s*farWorld\s*-\s*nearPoint\s*;/,
  "A1-G: vertex must compute the raw projective ray (farWorld - nearPoint)"
);
assert.match(
  vert,
  /farPoint\s*=\s*nearPoint\s*\+\s*dir\s*\*\s*RAY_SCALE\s*;/,
  "A1-G: vertex must pass the UN-normalized vector scaled by the fixed constant (NOT per-vertex normalize)"
);
assert.doesNotMatch(
  vert,
  /normalize\s*\(\s*dir\s*\)|normalize\s*\(\s*farWorld\s*-\s*nearPoint\s*\)/,
  "A1-G: vertex must never normalize the projective per-vertex ray (bends direction field)"
);
assert.match(
  shader,
  /vec3\s+rayDirection\s*=\s*rawLen\s*>\s*1e-9\s*\?\s*\(\s*rawDir\s*\/\s*rawLen\s*\)\s*:\s*vec3\s*\(\s*0\.0\s*,\s*-1\.0\s*,\s*0\.0\s*\)\s*;/,
  "C1/A1-G: the ONLY normalization is performed in the fragment after interpolation"
);
assert.match(
  shader,
  /bool\s+above\s*=\s*denom\s*<\s*-1e-6\s*;/,
  "C1: a ray descending onto the plane (denom < -1e-6) is the only valid intersection"
);
assert.match(
  shader,
  /bool\s+valid\s*=\s*above\s*&&\s*t\s*>=\s*0\.0\s*;/,
  "C1: underside / behind-camera intersections are rejected outright"
);
assert.match(
  shader,
  /valid\s*\?\s*1\.0\s*:\s*0\.0/,
  "C1: invalid (underside) fragments get zero alpha so no grid appears above the plane"
);

console.log("editor_grid_shader_tests: PASS");
