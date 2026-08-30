#!/usr/bin/env node
// GridShaderSourceGate.mjs — C6-GRID-ARTIFACT-001: prova EXECUTADA de que as
// fontes ativas (shaders/active/) compilam DETERMINISTICAMENTE para os hashes
// canônicos (out/dev-shared/shaders/). Fecha a cadeia fonte → SPIR-V → runtime
// sem build completo: compila cada shader do grid com glslc unitário para um
// temp dir e compara sha1 com o canônico. FAIL (exit 1) se qualquer fonte
// ativa divergir do canônico (regressão de fonte ou cópia fantasma re-introduzida).
//
// Uso: node tests/GridShaderSourceGate.mjs

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dirname, "..");
const SRC_DIR = join(ROOT, "shaders", "active");
const CANON_DIR = join(ROOT, "out", "dev-shared", "shaders");

// glslc do Vulkan SDK (o mesmo que o CMake usa via GLSLC_EXECUTABLE).
const GLSLC_CANDIDATES = [
  "C:/VulkanSDK/1.4.341.1/Bin/glslc.exe",
  "C:/VulkanSDK/1.3.290.0/Bin/glslc.exe",
  "C:/VulkanSDK/1.3.275.0/Bin/glslc.exe",
];
const glslc = GLSLC_CANDIDATES.find((p) => existsSync(p));
if (!glslc) {
  console.error("GridShaderSourceGate: glslc not found (Vulkan SDK). Cannot prove determinism.");
  process.exit(1);
}

// MESMA lista VC_SHADERS do CMakeLists.txt (26 shaders) — prova que a árvore
// canônica INTEIRA é determinística a partir das fontes ativas, não só o grid.
const SHADERS = [
  "voxel.vert", "far_surface.vert", "voxel.frag", "grass.vert", "foliage.vert",
  "sky.vert", "sky.frag", "post.vert", "post.frag", "shadow.vert",
  "shadow_far_surface.vert", "shadow.frag", "shadow_foliage.vert", "shadow_grass.vert",
  "editor_viewport.vert", "editor_viewport.frag", "editor_pick.frag", "editor_material.vert",
  "editor_grid.vert", "editor_grid.frag", "editor_splat.vert", "editor_splat.frag",
  "editor_envsphere.vert", "editor_envsphere.frag", "block.vert", "block.frag",
];

function sha1(file) {
  return createHash("sha1").update(readFileSync(file)).digest("hex");
}

let fails = 0;
const tmp = mkdtempSync(join(tmpdir(), "vc-gridsrc-"));
for (const name of SHADERS) {
  const src = join(SRC_DIR, name);
  if (!existsSync(src)) {
    console.error(`FAIL ${name}: source missing ${src}`);
    fails++;
    continue;
  }
  const out = join(tmp, `${name}.spv`);
  try {
    execFileSync(glslc, ["-I", SRC_DIR, src, "-o", out], { stdio: "pipe" });
  } catch (e) {
    console.error(`FAIL ${name}: glslc error: ${e.stderr?.toString() || e.message}`);
    fails++;
    continue;
  }
  const got = sha1(out).slice(0, 8);
  const canonFile = join(CANON_DIR, `${name}.spv`);
  const canon = existsSync(canonFile) ? sha1(canonFile).slice(0, 8) : "MISSING";
  const ok = got === canon;
  console.log(`${ok ? "OK  " : "FAIL"} ${name}: compiled=${got} canonical=${canon}`);
  if (!ok) fails++;
}
rmSync(tmp, { recursive: true, force: true });

if (fails > 0) {
  console.error(`GridShaderSourceGate: FAIL (${fails})`);
  process.exit(1);
}
console.log(`GridShaderSourceGate: PASS (${SHADERS.length} active sources compile deterministically to canonical SPIR-V hashes)`);
process.exit(0);
