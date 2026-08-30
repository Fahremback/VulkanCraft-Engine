#!/usr/bin/env node
// Semantic Engine API CLI (task_plan Agente 5 §4/§8 — "fachada semântica única
// para editor, CLI, MCP e automação" + "cada capacidade pública acessível
// coerentemente por C++, reflection, scripting visual, CLI e MCP"). The
// Semantic Engine API has 65 tools (auto-derived from semanticToolDefinitions);
// registry-cli.mjs only exposes the 28 asset-authoring kinds (6 registry + the
// vehicle/ability/mission/world_profile/gait/simulation_lod/prefab/particle
// kinds + 13 config kinds incl. network). This CLI exposes the FULL semantic surface WITHOUT a
// running MCP server — every command goes through the SAME factories
// (callSemanticTool in game-authoring.mjs) the MCP server uses, so the CLI can
// never drift from what the MCP surface accepts.
//
// Usage:
//   node semantic-cli.mjs tools                          list all 65 semantic tools + schemas
//   node semantic-cli.mjs schema <tool>                  print the input schema for a tool
//   node semantic-cli.mjs call <tool> '<json-args>' [--engine <root>]
//                                                       call a tool (exit 0 success, 2 isError, 3 driver error)
import path from "node:path";
import { fileURLToPath } from "node:url";
import { callSemanticTool, semanticToolDefinitions } from "./game-authoring.mjs";

const SERVER_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(SERVER_DIR, "..", "..");
const DEFS = semanticToolDefinitions();

function cmdTools() {
  for (const def of DEFS) {
    console.log(`${def.name}\t${def.description}`);
  }
  console.error(`${DEFS.length} semantic tools`);
}

function cmdSchema(toolName) {
  const def = DEFS.find((d) => d.name === toolName);
  if (!def) {
    console.error(`unknown semantic tool '${toolName}' (see 'tools')`);
    process.exit(2);
  }
  console.log(JSON.stringify(def.inputSchema, null, 2));
}

function cmdCall(toolName, args, engineRoot) {
  const def = DEFS.find((d) => d.name === toolName);
  if (!def) {
    console.error(`unknown semantic tool '${toolName}' (see 'tools')`);
    process.exit(3);
  }
  let parsed = {};
  if (args !== undefined) {
    try {
      parsed = JSON.parse(args);
    } catch (error) {
      console.error(`args JSON inválido: ${error.message}`);
      process.exit(3);
    }
  }
  let result;
  try {
    result = callSemanticTool(engineRoot, toolName, parsed);
  } catch (error) {
    result = { isError: true, message: error instanceof Error ? error.message : String(error) };
  }
  if (result === undefined) {
    console.error(`semantic tool '${toolName}' returned no result (internal)`);
    process.exit(3);
  }
  console.log(JSON.stringify(result, null, 2));
  process.exit(result.isError ? 2 : 0);
}

function main() {
  const argv = process.argv.slice(2);
  const engineFlag = argv.indexOf("--engine");
  const engineRoot = engineFlag >= 0 && argv[engineFlag + 1] ? argv[engineFlag + 1] : ENGINE_ROOT;
  const positional = engineFlag >= 0
    ? argv.filter((_, i) => i !== engineFlag && i !== engineFlag + 1)
    : argv.slice();
  const [command, ...rest] = positional;
  switch (command) {
    case "tools": cmdTools(); break;
    case "schema": cmdSchema(rest[0]); break;
    case "call": cmdCall(rest[0], rest[1], engineRoot); break;
    default:
      console.error(
        "Usage: node semantic-cli.mjs <tools|schema|call> ...\n" +
        "  tools                    list all semantic tools + descriptions\n" +
        "  schema <tool>            print the input JSON Schema for a tool\n" +
        "  call <tool> '<json>'     call a tool (same factories as the MCP server)\n" +
        "                           exit: 0 success, 2 tool isError, 3 driver error\n"
      );
      process.exit(3);
  }
}

main();
