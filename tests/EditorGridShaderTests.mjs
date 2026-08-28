import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const shaderPath = resolve(here, "../shaders/active/editor_grid.frag");
const shader = readFileSync(shaderPath, "utf8");

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

console.log("editor_grid_shader_tests: PASS");
