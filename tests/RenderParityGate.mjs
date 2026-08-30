// RenderParityGate.mjs — executable parity gate (A1-14/84/102, Agente 2).
//
// The parity contract: BOTH executables must rasterize PBR material-graph
// content through the SAME shared compiler contract
// `Rendering::compile_glsl_to_spirv` (the public IShaderCompiler slang core,
// declared in the single shared header MaterialPipeline.hpp), NOT through a
// private duplicated `std::system("glslc ...")` path.
//
//   - Game:  material_graph_from_pbr → material_graph_to_glsl →
//            Rendering::compile_glsl_to_spirv        (direct, shared symbols)
//   - Editor: material_graph_to_glsl → compile_material_glsl →
//            Engine::Rendering::compile_glsl_to_spirv  (delegates to the same
//            shared compiler; no private system() path, no duplicate definition)
//
// This gate fails (exit 1) if either side drops the shared compiler contract.
// It is a build-free executable proxy for the "same material graph → same GLSL
// → same SPIR-V" acceptance.
//
//   node tests/RenderParityGate.mjs      # → RenderParityGate: PASS | FAIL

import { readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");

const SHARED_HEADER = resolve(root, "src/engine/rendering/vulkan/MaterialPipeline.hpp");

let fails = 0;

// 1) The shared header must declare the compiler contract.
if (!existsSync(SHARED_HEADER)) {
  console.error(`  ✗ shared header missing: ${SHARED_HEADER}`);
  fails++;
} else {
  const h = readFileSync(SHARED_HEADER, "utf8");
  for (const sym of ["material_graph_to_glsl", "compile_glsl_to_spirv"]) {
    if (!h.includes(sym)) { console.error(`  ✗ shared header no longer declares ${sym}`); fails++; }
  }
  if (fails === 0) console.log("  ✓ shared header declares material_graph_to_glsl + compile_glsl_to_spirv");
}

const hasAny = (files, needle) => files.some((f) => existsSync(f) && readFileSync(f, "utf8").includes(needle));

// 2) Editor must NOT keep a private duplicated system() glslc path in any TU.
const editorTUs = [
  resolve(root, "src/editor/EditorApplicationPanels.cpp"),
  resolve(root, "src/editor/EditorApplicationRecovered.cpp"),
  resolve(root, "src/editor/EditorApplicationAssets.cpp"),
  resolve(root, "src/editor/EditorApplicationVulkan.cpp"),
];
for (const f of editorTUs) {
  if (!existsSync(f)) continue;
  const t = readFileSync(f, "utf8");
  if (/vc_editor_material_tmp/.test(t)) {
    console.error(`  ✗ editor private glslc path still present in ${f.split(/[\\/]/).pop()} (compile_material_glsl must delegate)`);
    fails++;
  }
}
if (fails === 0) console.log("  ✓ no editor TU keeps the private duplicated glslc path (vc_editor_material_tmp)");

// 3) Editor compile_material_glsl must delegate to the shared contract.
if (hasAny(editorTUs, "compile_material_glsl")) {
  const panels = readFileSync(resolve(root, "src/editor/EditorApplicationPanels.cpp"), "utf8");
  if (!/compile_material_glsl[\s\S]{0,600}compile_glsl_to_spirv/m.test(panels)) {
    console.error("  ✗ editor compile_material_glsl (Panels.cpp) no longer delegates to Rendering::compile_glsl_to_spirv");
    fails++;
  }
  const recovered = readFileSync(resolve(root, "src/editor/EditorApplicationRecovered.cpp"), "utf8");
  if (!/compile_material_glsl[\s\S]{0,600}compile_glsl_to_spirv/m.test(recovered)) {
    console.error("  ✗ editor compile_material_glsl (Recovered.cpp) no longer delegates to Rendering::compile_glsl_to_spirv");
    fails++;
  }
  if (fails === 0) console.log("  ✓ both editor compile_material_glsl definitions delegate to Rendering::compile_glsl_to_spirv");
} else {
  console.error("  ✗ editor has no compile_material_glsl at all");
  fails++;
}

// 4) Game must use the shared generator + shared compiler.
const gameTUs = [
  resolve(root, "src/app/VulkanGameRender.cpp"),
  resolve(root, "src/app/VulkanEngineApp.cpp"),
];
const gameText = gameTUs.filter((f) => existsSync(f)).map((f) => readFileSync(f, "utf8")).join("\n");
const missingGame = ["material_graph_from_pbr", "material_graph_to_glsl", "compile_glsl_to_spirv"].filter((s) => !gameText.includes(s));
if (missingGame.length > 0) {
  console.error(`  ✗ game missing shared generator symbols: ${missingGame.join(", ")}`);
  fails++;
} else {
  console.log("  ✓ game routes PBR material-graph through material_graph_from_pbr → material_graph_to_glsl → compile_glsl_to_spirv");
}

if (fails > 0) {
  console.error("RenderParityGate: FAIL — a product executable diverged from the shared PBR compiler contract.");
  process.exit(1);
}
console.log("RenderParityGate: PASS (game and editor share the same PBR material-graph → GLSL → SPIR-V compiler contract)");
process.exit(0);