#!/usr/bin/env node
// tool-wiring.mjs — task_plan Agente 5 §8 item 1 (linha 114): o WIRING
// por-tool da fachada semântica para reflection e scripting visual.
//
// A matriz de capacidades (capability-matrix.mjs) sempre teve C++/MCP/CLI
// coerentes por construção (as mesmas factories) mas reflection e scripting
// visual marcados como "sem mapeamento por-tool". Este gerador FECHA esse
// gap SEM lista manual: cada tool de `semanticToolDefinitions()` (fonte
// única) é projetada deterministicamente em:
//
//   1. REFLECTION — um TypeInfo no formato EXATO que `IReflection::
//      load_from_json` (src/engine/sdk/Reflection.cpp) parseia:
//      `{"version":1,"types":[{name,stable_id,alias,version,fields:[
//      {name,kind,value_type,default}]}]}`. Kinds: string/enum/int/float/
//      bool/array/json — o subconjunto fiel do JSON Schema das tools.
//
//   2. SCRIPTING VISUAL — um NodeDef no formato EXATO que
//      `IVisualScriptGraph::register_node_type` consome (type_name,
//      category, description, inputs/outputs PinDef, reflection_type).
//      O `reflection_type` de cada nó aponta para o TypeInfo do item 1 —
//      o cross-link reflection⇄scripting que o grafo visual usa.
//
// Saídas COMMITTED (a prova anti-drift):
//   schema/reflection/tools.json      — documento IReflection carregável
//   schema/scripting/visual-nodes.json — defs de nó do grafo visual
//
// COMPLETUDE: exit 1 se qualquer tool da fachada não produzir TypeInfo E
// NodeDef — o wiring não pode ficar pela metade.
// DETERMINÍSTICO: sem timestamps, tools ordenadas, JSON estável.
//
// Usage: node tools/sdk/tool-wiring.mjs [--check]
//   (--check apenas valida os artifacts committed contra a fonte única)
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { semanticToolDefinitions } from "../mcp-server/game-authoring.mjs";

const ENGINE_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const REFLECTION_ARTIFACT = path.join(ENGINE_ROOT, "schema", "reflection", "tools.json");
const VISUAL_ARTIFACT = path.join(ENGINE_ROOT, "schema", "scripting", "visual-nodes.json");

// ---- Categorias do NodeDef derivadas do prefixo da tool -------------------
function categoryOf(name) {
  const first = name.split("_")[0];
  switch (first) {
    case "game": return "project";
    case "list": case "inspect": return "query";
    case "create": case "author": case "instantiate": case "stage": return "authoring";
    case "set": case "remove": case "apply": return "mutation";
    case "validate": return "validation";
    default: return "semantic";
  }
}

// ---- Mapeamentos JSON Schema -> IReflection / PinType ---------------------
// FieldKind válido para load_from_json: int/float/bool/string/enum/array/json.
function reflectionKind(schema) {
  switch (schema.type) {
    case "integer": return "int";
    case "number": return "float";
    case "boolean": return "bool";
    case "string": return schema.enum ? "enum" : "string";
    case "array": return "array";
    case "object": return "json";
    default: return "json";
  }
}

function reflectionValueType(schema) {
  switch (schema.type) {
    case "integer": case "number": return schema.type;
    case "boolean": return "boolean";
    case "string": return schema.enum ? JSON.stringify(schema.enum) : "string";
    case "array": {
      const items = schema.items && schema.items.type ? schema.items.type : "string";
      return `array<${items}>`;
    }
    case "object": return "object";
    default: return "string";
  }
}

function reflectionDefault(schema) {
  if (schema.default === undefined) return "";
  if (typeof schema.default === "string") return schema.default;
  return JSON.stringify(schema.default);
}

// PinType do grafo visual: Bool/Int/Float/String/Vec2/Vec3/Vec4/Entity/
// Asset/Event/Any.
function pinType(schema) {
  switch (schema.type) {
    case "integer": return "Int";
    case "number": return "Float";
    case "boolean": return "Bool";
    case "string": return "String";
    case "array": return "Any";
    case "object": return "Any";
    default: return "Any";
  }
}

// ---- Projeção de UMA tool ------------------------------------------------
function reflectTool(def) {
  const fields = [];
  const props = (def.inputSchema && def.inputSchema.properties) || {};
  for (const [name, schema] of Object.entries(props)) {
    const field = { name, kind: reflectionKind(schema) };
    const valueType = reflectionValueType(schema);
    if (valueType) field.value_type = valueType;
    const deflt = reflectionDefault(schema);
    if (deflt) field.default = deflt;
    fields.push(field);
  }
  return {
    name: def.name,
    stable_id: `semantic.${def.name}`,
    alias: "",
    version: "1.0.0",
    fields
  };
}

function wireNode(def) {
  const inputs = [];
  const props = (def.inputSchema && def.inputSchema.properties) || {};
  const required = new Set((def.inputSchema && def.inputSchema.required) || []);
  for (const [name, schema] of Object.entries(props)) {
    const pin = { name, type: pinType(schema), required: required.has(name) };
    const deflt = reflectionDefault(schema);
    if (deflt) pin.default = deflt;
    inputs.push(pin);
  }
  return {
    type_name: def.name,
    category: categoryOf(def.name),
    description: (def.description || "").split("\n")[0].slice(0, 120),
    inputs,
    outputs: [{ name: "result", type: "Any", required: false }],
    reflection_type: `semantic.${def.name}`
  };
}

// ---- Geração determinística ----------------------------------------------
function generate() {
  const defs = [...semanticToolDefinitions()].sort((a, b) => a.name.localeCompare(b.name));

  // Completude: toda tool precisa de TypeInfo E NodeDef.
  const missing = [];
  const types = [];
  const nodes = [];
  for (const def of defs) {
    const type = reflectTool(def);
    const node = wireNode(def);
    if (!type || !type.name) { missing.push(def.name); continue; }
    if (!node || !node.type_name || !node.reflection_type) { missing.push(def.name); continue; }
    types.push(type);
    nodes.push(node);
  }
  if (missing.length) {
    console.error(`TOOL WIRING INCOMPLETE — ${missing.length} tool(s) sem TypeInfo/NodeDef: ${missing.join(", ")}`);
    process.exitCode = 1;
  }

  const reflectionDoc = { version: 1, types };
  const visualDoc = { version: 1, nodes };
  return { reflectionDoc, visualDoc, toolCount: defs.length };
}

function writeArtifacts() {
  const { reflectionDoc, visualDoc, toolCount } = generate();
  fs.mkdirSync(path.dirname(REFLECTION_ARTIFACT), { recursive: true });
  fs.mkdirSync(path.dirname(VISUAL_ARTIFACT), { recursive: true });
  fs.writeFileSync(REFLECTION_ARTIFACT, JSON.stringify(reflectionDoc, null, 2) + "\n", "utf8");
  fs.writeFileSync(VISUAL_ARTIFACT, JSON.stringify(visualDoc, null, 2) + "\n", "utf8");
  console.log(`tool-wiring: wrote ${toolCount} reflection types -> schema/reflection/tools.json`);
  console.log(`tool-wiring: wrote ${toolCount} visual nodes   -> schema/scripting/visual-nodes.json`);
  return toolCount;
}

// --check: compara os artifacts committed com a regeneração (anti-drift).
function checkArtifacts() {
  const { reflectionDoc, visualDoc, toolCount } = generate();
  const freshRefl = JSON.stringify(reflectionDoc, null, 2) + "\n";
  const freshVisual = JSON.stringify(visualDoc, null, 2) + "\n";
  let failures = 0;
  if (!fs.existsSync(REFLECTION_ARTIFACT)) {
    console.error("TOOL WIRING STALE — schema/reflection/tools.json não existe (rode o gerador)");
    failures++;
  } else if (fs.readFileSync(REFLECTION_ARTIFACT, "utf8") !== freshRefl) {
    console.error("TOOL WIRING STALE — schema/reflection/tools.json difere da regeneração (rode `node tools/sdk/tool-wiring.mjs`)");
    failures++;
  }
  if (!fs.existsSync(VISUAL_ARTIFACT)) {
    console.error("TOOL WIRING STALE — schema/scripting/visual-nodes.json não existe (rode o gerador)");
    failures++;
  } else if (fs.readFileSync(VISUAL_ARTIFACT, "utf8") !== freshVisual) {
    console.error("TOOL WIRING STALE — schema/scripting/visual-nodes.json difere da regeneração (rode o gerador)");
    failures++;
  }
  if (failures) process.exitCode = 1;
  else console.log(`tool-wiring --check: OK (${toolCount} tools wired em reflection + scripting visual, anti-drift)`);
}

const check = process.argv.includes("--check");
if (check) checkArtifacts();
else writeArtifacts();
