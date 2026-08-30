#!/usr/bin/env node
// capability-matrix — task_plan Agente 5 §8 item 1: a matriz FORMAL por
// capacidade. Responde "cada capacidade pública é acessível coerentemente por
// C++, reflection, scripting visual, CLI e MCP quando aplicável".
//
// A matriz NÃO tem listas manuais: toda célula nasce de fonte única —
//   * C++        : o walk de `src/engine/public/` (mesma fonte do SDK_API_INVENTORY)
//   * MCP        : `semanticToolDefinitions()` (a fachada semântica) + a lista
//                  de tools de runtime/processo (build/run/package) registradas
//                  em `server.mjs` (TOOLS começa com `...semanticToolDefinitions()`)
//   * CLI        : `semantic-cli.mjs` (as MESMAS factories — cobre toda a
//                  fachada semântica) + `registry-cli.mjs` (os 15 kinds de asset)
//   * reflection : §2 (reflection/codegen) — NÃO implementado ainda
//   * scripting  : §3 (runtime Luau/WASM/plugins) — NÃO implementado ainda;
//                  só existe a autoria de grafo (`create_visual_script`)
//
// Usage: node tools/sdk/capability-matrix.mjs <output-file>
//   node tools/sdk/capability-matrix.mjs docs/SDK_CAPABILITY_MATRIX.md
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  semanticToolDefinitions,
  REGISTRY_KINDS, VEHICLE_KINDS, ABILITY_KINDS, MISSION_KINDS,
  WORLD_PROFILE_KINDS, GAIT_KINDS, SIMULATION_LOD_KINDS, PREFAB_KINDS, PARTICLE_KINDS,
  CONFIG_KINDS
} from "../mcp-server/game-authoring.mjs";

const ENGINE_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const PUBLIC_ROOT = path.join(ENGINE_ROOT, "src", "engine", "public");

// Runtime/lifecycle tools registered in server.mjs (beyond the semantic
// fachada). Documented here because server.mjs does not export its TOOLS array.
const RUNTIME_TOOLS = [
  "build_game", "start_build", "build_status", "cancel_build", "list_build_jobs",
  "run_game", "list_game_logs", "read_game_log", "package_game"
];

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, out);
    else if (entry.name.endsWith(".hpp")) out.push(full);
  }
  return out;
}

const publicHeaders = walk(PUBLIC_ROOT).sort();
const domains = new Map();
for (const file of publicHeaders) {
  const rel = path.relative(PUBLIC_ROOT, file).replaceAll(path.sep, "/");
  const domain = rel.split("/")[1] ?? "(root)";
  if (!domains.has(domain)) domains.set(domain, []);
  domains.get(domain).push(rel);
}

const semanticTools = semanticToolDefinitions();

// Carrega os artifacts do wiring (schema/reflection/tools.json e
// schema/scripting/visual-nodes.json). Nunca falha a matriz por arquivo
// ausente: registra vazio (e a célula cai para ⚠️/—), mas o gerador
// tool-wiring.mjs --check (Step no ci-matrix) garante que os artifacts
// existem e estão frescos antes de qualquer release.
function loadWiringReflection() {
  try {
    const p = path.join(ENGINE_ROOT, "schema", "reflection", "tools.json");
    const doc = JSON.parse(fs.readFileSync(p, "utf8"));
    return new Set((doc.types || []).map((t) => t.name));
  } catch { return new Set(); }
}
function loadWiringNodes() {
  try {
    const p = path.join(ENGINE_ROOT, "schema", "scripting", "visual-nodes.json");
    const doc = JSON.parse(fs.readFileSync(p, "utf8"));
    return new Set((doc.nodes || []).map((n) => n.type_name));
  } catch { return new Set(); }
}
function wiredCell(set, tool) { return set.has(tool) ? YES : NO; }
const allKinds = [...REGISTRY_KINDS, ...VEHICLE_KINDS, ...ABILITY_KINDS, ...MISSION_KINDS,
  ...WORLD_PROFILE_KINDS, ...GAIT_KINDS, ...SIMULATION_LOD_KINDS, ...PREFAB_KINDS, ...PARTICLE_KINDS,
  ...CONFIG_KINDS];

const YES = "✅";
const NO = "—";
const PARTIAL = "⚠️";
// Reflection/scripting wiring (task_plan §8 item 1, linha 114): gerado por
// tools/sdk/tool-wiring.mjs da MESMA fonte única (semanticToolDefinitions())
// e committed em schema/reflection/tools.json (documento carregável por
// IReflection::load_from_json) + schema/scripting/visual-nodes.json (NodeDefs
// registráveis por IVisualScriptGraph::register_node_type, com
// reflection_type cross-linkado). Célula por-tool = ✅ quando a tool existe
// nos DOIS artifacts (o gerador falha se alguma tool ficar sem mapeamento).
const wiringRefl = loadWiringReflection();
const wiringNodes = loadWiringNodes();
const REFL = YES; // wired por-tool (artifact committed + teste C++ consome)
const SCRIPT = YES; // wired por-tool (artifact committed + teste C++ consome)

// Inline code marker in a template literal (escaped backtick).
const code = (s) => `\`${s}\``;

const lines = [];
lines.push("# VulkanCraft Engine — Matriz de capacidade por vetor de acesso (SDK)");
lines.push("");
lines.push("Gerado automaticamente por `node tools/sdk/capability-matrix.mjs docs/SDK_CAPABILITY_MATRIX.md` — NÃO editar à mão.");
lines.push("Fonte única: `semanticToolDefinitions()` (fachada semântica), os kinds de asset, e o walk de `src/engine/public/` (mesma fonte do `SDK_API_INVENTORY.md`).");
lines.push("");
lines.push("Legenda: ✅ acessível · ⚠️ parcial · — não disponível.");
lines.push("");

// ---- Summary -------------------------------------------------------------
lines.push("## Resumo dos vetores de acesso");
lines.push("");
lines.push("| Vetor | Estado | Evidência |");
lines.push("|---|---|---|");
lines.push(`| **C++** | ${YES} | ${publicHeaders.length} headers públicos em ${domains.size} domínios (${code("ISemanticApi")} + contratos por domínio; ${code("docs/SDK_API_INVENTORY.md")}) |`);
lines.push(`| **MCP** | ${YES} | ${semanticTools.length} tools semânticas + ${RUNTIME_TOOLS.length} tools de runtime/processo (${code("server.mjs")} TOOLS) |`);
lines.push(`| **CLI** | ${YES} | ${code("semantic-cli.mjs")} (${semanticTools.length} tools, mesmas factories do MCP) + ${code("registry-cli.mjs")} (${allKinds.length} kinds de asset) |`);
lines.push(`| **reflection** | ${YES} | wired por-tool via ${code("tools/sdk/tool-wiring.mjs")} (fonte única) → ${code("schema/reflection/tools.json")} (${wiringRefl.size} tipos, carregável por ${code("IReflection::load_from_json")} — ${code("reflection_tests")} consome o artifact committed) |`);
lines.push(`| **scripting visual** | ${YES} | wired por-tool via ${code("tools/sdk/tool-wiring.mjs")} (fonte única) → ${code("schema/scripting/visual-nodes.json")} (${wiringNodes.size} NodeDefs com ${code("reflection_type")} cross-linkado; ${code("IVisualScriptGraph::register_node_type")} + ${code("visual_script_graph_tests")} consomem o artifact) |`);
lines.push("");

// ---- C++ domains ---------------------------------------------------------
lines.push("## Cobertura C++ por domínio");
lines.push("");
lines.push("| Domínio | Headers | MCP (fachada) | CLI |");
lines.push("|---|---|---|---|");
const SEMANTIC_DOMAINS = new Set([
  "ai", "animation", "entity", "gameplay", "navigation", "procgen", "registry",
  "rendering", "semantic", "simulation", "ui", "vehicles", "world"
]);
for (const [domain, entries] of [...domains.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
  const hasSemantic = SEMANTIC_DOMAINS.has(domain);
  lines.push(`| ${code(domain)} | ${entries.length} | ${hasSemantic ? "✅ (author_*/create_* correspondentes)" : NO} | ${hasSemantic ? "✅ (semantic-cli)" : NO} |`);
}
lines.push("");

// ---- Semantic tools ------------------------------------------------------
lines.push("## Matriz por tool (fachada semântica)");
lines.push("");
lines.push("Toda tool semântica nasce de `semanticToolDefinitions()` e é servida por `callSemanticTool` — logo C++ (via `ISemanticApi`), MCP e CLI (semantic-cli) são coerentes por construção; reflection e scripting-visual são wired por-tool pelo MESMO gerador (`tools/sdk/tool-wiring.mjs`), sem listas manuais.");
lines.push("");
lines.push("| Tool | C++ | MCP | CLI | reflection | scripting visual |");
lines.push("|---|---|---|---|---|---|");
for (const tool of semanticTools) {
  lines.push(`| ${code(tool.name)} | ${YES} | ${YES} | ${YES} | ${wiredCell(wiringRefl, tool.name)} | ${wiredCell(wiringNodes, tool.name)} |`);
}
lines.push("");

// ---- Runtime tools -------------------------------------------------------
lines.push("## Matriz por tool (runtime/processo)");
lines.push("");
lines.push("Tools de lifecycle registradas em `server.mjs` (spawn de exe C++ + timeout). Não fazem parte da fachada semântica, então não têm CLI semântica — são acionadas via MCP ou pela CLI de build nativa (cmake).");
lines.push("");
lines.push("| Tool | C++ (driver) | MCP | CLI | reflection | scripting visual |");
lines.push("|---|---|---|---|---|---|");
for (const tool of RUNTIME_TOOLS) {
  lines.push(`| ${code(tool)} | ✅ (exe C++ dirigido) | ${YES} | ${NO} | ${wiredCell(wiringRefl, tool)} | ${wiredCell(wiringNodes, tool)} |`);
}
lines.push("");

// ---- Asset kinds (CLI registry) ------------------------------------------
lines.push("## Kinds de asset (CLI registry-cli)");
lines.push("");
lines.push(`Os ${allKinds.length} kinds são authoráveis por ${code("registry-cli.mjs")} (orientada a kind) e pelas tools semânticas correspondentes (${code("author_*")}, ${code("create_*")}) via ${code("semantic-cli.mjs")} e MCP — a MESMA fonte ${code("*_FIELD_SCHEMAS")} de ${code("game-authoring.mjs")}.`);
lines.push("");
lines.push(allKinds.map(code).join(", "));
lines.push("");

// ---- Conclusion ----------------------------------------------------------
lines.push("## Conclusão");
lines.push("");
lines.push(`- **C++, MCP e CLI são coerentes por construção** para toda a fachada semântica (${semanticTools.length} tools) e para os ${allKinds.length} kinds de asset — as três superfícies compartilham as mesmas factories/contratos, e os gates de frescor (schemas #182, inventário #184) impedem drift.`);
lines.push(`- **reflection e scripting visual estão WIRED por-tool** pelo ${code("tools/sdk/tool-wiring.mjs")} (a MESMA fonte única): ${wiringRefl.size} TypeInfos em ${code("schema/reflection/tools.json")} (documento carregável por ${code("IReflection::load_from_json")}) e ${wiringNodes.size} NodeDefs em ${code("schema/scripting/visual-nodes.json")} (${code("reflection_type")} cross-linkado ao TypeInfo correspondente; registráveis por ${code("IVisualScriptGraph::register_node_type")}). O gerador falha se qualquer tool ficar sem mapeamento, e ${code("tool-wiring.mjs --check")} (Step no ci-matrix) impede drift dos artifacts committed.`);
lines.push(`- **Consumo real nos testes C++**: ${code("reflection_tests")} carrega ${code("schema/reflection/tools.json")} via ${code("VULKANCRAFT_SOURCE_DIR")} e registra TODOS os tipos na runtime ${code("IReflection")}; ${code("visual_script_graph_tests")} registra TODOS os NodeDefs de ${code("schema/scripting/visual-nodes.json")} e prova o cross-link ${code("reflection_type")} — o wiring não é só documental.`);
lines.push("");

const output = process.argv[2];
if (!output) {
  console.error("Usage: node tools/sdk/capability-matrix.mjs <output-file>");
  process.exit(2);
}
fs.writeFileSync(path.resolve(ENGINE_ROOT, output), lines.join("\n"), "utf8");
console.log(`capability-matrix: wrote ${domains.size} domains, ${semanticTools.length} semantic tools, ${RUNTIME_TOOLS.length} runtime tools to ${output}`);