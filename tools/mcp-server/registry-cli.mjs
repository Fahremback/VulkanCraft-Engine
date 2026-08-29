#!/usr/bin/env node
// Registry/vehicle/ability/mission asset CLI (FALTANTES item 10 + §17 item 12
// + §19 + item 23): expose the complete registry, vehicle-assembly, ability
// AND mission schemas and validate/author assets WITHOUT a running MCP
// server. Every command goes
// through the SAME factories the MCP server uses (game-authoring.mjs), so the
// CLI can never drift from what the mirror validation accepts — and the
// exported JSON Schemas are generated from the same FIELD_SCHEMAS contracts.
//
// Usage:
//   node registry-cli.mjs kinds
//   node registry-cli.mjs schema <kind>
//   node registry-cli.mjs export-schemas [outDir]        (default: <engine>/schema)
//   node registry-cli.mjs validate <kind> <file.json>
//   node registry-cli.mjs author --engine <root> --project <name> --kind <kind>
//                                --name <n> --file <doc.json> [--dry-run] [--update]
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  REGISTRY_KINDS,
  VEHICLE_KINDS,
  ABILITY_KINDS,
  MISSION_KINDS,
  WORLD_PROFILE_KINDS,
  GAIT_KINDS,
  SIMULATION_LOD_KINDS,
  PREFAB_KINDS,
  PARTICLE_KINDS,
  CONFIG_KINDS,
  buildRegistryJsonSchema,
  buildVehicleJsonSchema,
  buildAbilityJsonSchema,
  buildMissionJsonSchema,
  buildWorldProfileJsonSchema,
  buildGaitJsonSchema,
  buildSimulationLodJsonSchema,
  buildPrefabJsonSchema,
  buildParticleJsonSchema,
  buildConfigJsonSchema,
  validateRegistryDocument,
  validateVehicleDocument,
  validateAbilityDocument,
  validateMissionDocument,
  validateWorldProfileDocument,
  validateGaitDocument,
  validateSimulationLodDocument,
  validatePrefabDocument,
  validateParticleDocument,
  validateConfigDocument
} from "./game-authoring.mjs";

const SERVER_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(SERVER_DIR, "..", "..");
const ALL_KINDS = [...REGISTRY_KINDS, ...VEHICLE_KINDS, ...ABILITY_KINDS, ...MISSION_KINDS, ...WORLD_PROFILE_KINDS, ...GAIT_KINDS, ...SIMULATION_LOD_KINDS, ...PREFAB_KINDS, ...PARTICLE_KINDS, ...CONFIG_KINDS];
const VEHICLE_KIND_SET = new Set(VEHICLE_KINDS);
const ABILITY_KIND_SET = new Set(ABILITY_KINDS);
const MISSION_KIND_SET = new Set(MISSION_KINDS);
const WORLD_PROFILE_KIND_SET = new Set(WORLD_PROFILE_KINDS);
const GAIT_KIND_SET = new Set(GAIT_KINDS);
const SIMULATION_LOD_KIND_SET = new Set(SIMULATION_LOD_KINDS);
const PREFAB_KIND_SET = new Set(PREFAB_KINDS);
const PARTICLE_KIND_SET = new Set(PARTICLE_KINDS);
const CONFIG_KIND_SET = new Set(CONFIG_KINDS);

// §4.4/§4.5 config kinds — directory under Content/ where the CLI authors
// each kind (mirrors configDir in game-authoring.mjs).
const CONFIG_DIRS = Object.freeze({
  shader: "Shaders", render_graph: "RenderGraphs", light: "Lights", gi: "GI",
  ocean: "Ocean", post_process: "PostProcess", fluid_sim: "FluidSims",
  world: "Worlds", chunk: "Chunks", transaction: "Transactions",
  block_entity: "BlockEntities", inventory: "Inventories"
});

function fail(message, code = 1) {
  console.error(`registry-cli: ${message}`);
  process.exit(code);
}

function atomicWriteJson(filePath, value) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  const temp = path.join(
    path.dirname(filePath),
    `.${path.basename(filePath)}.${process.pid}.${crypto.randomUUID()}.tmp`
  );
  fs.writeFileSync(temp, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  try {
    fs.renameSync(temp, filePath);
  } catch (error) {
    try { fs.rmSync(temp, { force: true }); } catch {}
    throw error;
  }
}

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, "utf8"));
}

function cmdKinds() {
  console.log(ALL_KINDS.join("\n"));
}

function buildKindSchema(kind) {
  return VEHICLE_KIND_SET.has(kind)
    ? buildVehicleJsonSchema(kind)
    : ABILITY_KIND_SET.has(kind)
      ? buildAbilityJsonSchema(kind)
      : MISSION_KIND_SET.has(kind)
        ? buildMissionJsonSchema(kind)
        : WORLD_PROFILE_KIND_SET.has(kind)
          ? buildWorldProfileJsonSchema(kind)
          : GAIT_KIND_SET.has(kind)
            ? buildGaitJsonSchema(kind)
            : SIMULATION_LOD_KIND_SET.has(kind)
              ? buildSimulationLodJsonSchema(kind)
              : PREFAB_KIND_SET.has(kind)
                ? buildPrefabJsonSchema(kind)
                : PARTICLE_KIND_SET.has(kind)
                  ? buildParticleJsonSchema(kind)
                  : CONFIG_KIND_SET.has(kind)
                    ? buildConfigJsonSchema(kind)
                    : buildRegistryJsonSchema(kind);
}

function validateKindDocument(kind, document) {
  return VEHICLE_KIND_SET.has(kind)
    ? validateVehicleDocument(kind, document)
    : ABILITY_KIND_SET.has(kind)
      ? validateAbilityDocument(kind, document)
      : MISSION_KIND_SET.has(kind)
        ? validateMissionDocument(kind, document)
        : WORLD_PROFILE_KIND_SET.has(kind)
          ? validateWorldProfileDocument(kind, document)
          : GAIT_KIND_SET.has(kind)
            ? validateGaitDocument(kind, document)
            : SIMULATION_LOD_KIND_SET.has(kind)
              ? validateSimulationLodDocument(kind, document)
              : PREFAB_KIND_SET.has(kind)
                ? validatePrefabDocument(document)
                : PARTICLE_KIND_SET.has(kind)
                  ? validateParticleDocument(document)
                  : CONFIG_KIND_SET.has(kind)
                    ? validateConfigDocument(kind, document)
                    : validateRegistryDocument(kind, document);
}

function cmdSchema(kind) {
  if (!ALL_KINDS.includes(kind)) fail(`unsupported asset kind '${kind}' (supported: ${ALL_KINDS.join(", ")})`);
  console.log(JSON.stringify(buildKindSchema(kind), null, 2));
}

function cmdExportSchemas(outDir) {
  const target = outDir ?? path.join(ENGINE_ROOT, "schema");
  fs.mkdirSync(target, { recursive: true });
  const written = [];
  for (const kind of ALL_KINDS) {
    const file = path.join(target, `${kind}.json`);
    atomicWriteJson(file, buildKindSchema(kind));
    written.push(file);
  }
  for (const file of written) console.log(`wrote ${path.relative(ENGINE_ROOT, file).replaceAll(path.sep, "/")}`);
}

function cmdValidate(kind, file) {
  if (!ALL_KINDS.includes(kind)) fail(`unsupported asset kind '${kind}' (supported: ${ALL_KINDS.join(", ")})`);
  if (!file) fail("usage: validate <kind> <file.json>");
  let document;
  try {
    document = readJson(file);
  } catch (error) {
    fail(`cannot read '${file}': ${error.message}`);
  }
  const result = validateKindDocument(kind, document);
  if (!result.valid) {
    for (const diagnostic of result.errors) console.error(`- ${diagnostic}`);
    console.error(`registry-cli: '${file}' is INVALID (${result.errors.length} diagnostic(s))`);
    process.exit(1);
  }
  console.log(`registry-cli: '${file}' is valid (kind '${kind}')`);
}

function cmdAuthor(args) {
  const engine = args.engine ?? ENGINE_ROOT;
  const project = args.project;
  const kind = args.kind;
  const name = args.name;
  const file = args.file;
  const dryRun = args.dry_run === "true" || args.dry_run === true;
  const update = args.update === "true" || args.update === true;
  if (!project || !kind || !name || !file) {
    fail("author requires --project <name> --kind <kind> --name <n> --file <doc.json> [--engine <root>] [--dry-run] [--update]");
  }
  if (!ALL_KINDS.includes(kind)) fail(`unsupported asset kind '${kind}' (supported: ${ALL_KINDS.join(", ")})`);
  let document;
  try {
    document = readJson(file);
  } catch (error) {
    fail(`cannot read '${file}': ${error.message}`);
  }
  const validation = validateKindDocument(kind, document);
  if (!validation.valid) {
    for (const diagnostic of validation.errors) console.error(`- ${diagnostic}`);
    fail(`asset '${kind}/${name}' fails public-contract validation; nothing was written`);
  }
  const target = VEHICLE_KIND_SET.has(kind)
    ? path.join(engine, "Projects", project, "Content", "Vehicles", `${name}.json`)
    : ABILITY_KIND_SET.has(kind)
      ? path.join(engine, "Projects", project, "Content", "Abilities", `${name}.json`)
      : MISSION_KIND_SET.has(kind)
        ? path.join(engine, "Projects", project, "Content", "Missions", `${name}.json`)          : WORLD_PROFILE_KIND_SET.has(kind)
            ? path.join(engine, "Projects", project, "Content", "Profiles", `${name}.json`)
            : GAIT_KIND_SET.has(kind)
              ? path.join(engine, "Projects", project, "Content", "Animations", `${name}.json`)
              : SIMULATION_LOD_KIND_SET.has(kind)
                ? path.join(engine, "Projects", project, "Content", "SimulationLod", `${name}.json`)
                : PREFAB_KIND_SET.has(kind)
                  ? path.join(engine, "Projects", project, "Content", "Prefabs", `${name}.prefab`)
                  : PARTICLE_KIND_SET.has(kind)
                    ? path.join(engine, "Projects", project, "Content", "Particles", `${name}.particle`)
                    : CONFIG_KIND_SET.has(kind)
                      ? path.join(engine, "Projects", project, "Content", CONFIG_DIRS[kind], `${name}.json`)
                      : path.join(engine, "Projects", project, "Content", "Registry", kind, `${name}.json`);
  const relative = path.relative(path.join(engine, "Projects", project), target).replaceAll(path.sep, "/");
  const previous = fs.existsSync(target) ? readJson(target) : null;
  if (dryRun) {
    console.log(`[dry-run] would write ${relative}`);
    console.log(JSON.stringify(document, null, 2));
    return;
  }
  if (previous && !update) fail(`asset '${kind}/${name}' already exists in project '${project}' (pass --update to replace)`);
  atomicWriteJson(target, document);
  console.log(`wrote ${relative}`);
}

function main() {
  const [command, ...rest] = process.argv.slice(2);
  switch (command) {
    case "kinds": cmdKinds(); break;
    case "schema": cmdSchema(rest[0]); break;
    case "export-schemas": cmdExportSchemas(rest[0]); break;
    case "validate": cmdValidate(rest[0], rest[1]); break;
    case "author": {
      // Parse --key value pairs with bare flags (--dry-run) as true.
      const args = {};
      for (let i = 0; i < rest.length; i++) {
        if (!rest[i].startsWith("--")) continue;
        const key = rest[i].replace(/^--/, "").replace(/-/g, "_");
        const next = rest[i + 1];
        if (next !== undefined && !next.startsWith("--")) { args[key] = next; i++; }
        else args[key] = true;
      }
      cmdAuthor(args);
      break;
    }
    default:
      console.error(
        "Usage: node registry-cli.mjs <kinds|schema|export-schemas|validate|author> ...\n" +
        "  kinds                     list supported registry + vehicle + ability + mission + gait + simulation_lod + prefab + particle kinds\n" +
        "  schema <kind>             print the JSON Schema (draft-07) for a kind\n" +
        "  export-schemas [outDir]   write <kind>.json for every kind\n" +
        "  validate <kind> <file>    validate an asset document (exit 1 on invalid)\n" +
        "  author --project <name> --kind <kind> --name <n> --file <doc.json> [--engine <root>] [--dry-run] [--update]\n"
      );
      process.exit(2);
  }
}

main();
