#!/usr/bin/env node
// Registry/vehicle/ability asset CLI (FALTANTES item 10 + §17 item 12 + §19):
// expose the complete registry, vehicle-assembly AND ability schemas and
// validate/author assets WITHOUT a running MCP server. Every command goes
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
  buildRegistryJsonSchema,
  buildVehicleJsonSchema,
  buildAbilityJsonSchema,
  validateRegistryDocument,
  validateVehicleDocument,
  validateAbilityDocument
} from "./game-authoring.mjs";

const SERVER_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(SERVER_DIR, "..", "..");
const ALL_KINDS = [...REGISTRY_KINDS, ...VEHICLE_KINDS, ...ABILITY_KINDS];
const VEHICLE_KIND_SET = new Set(VEHICLE_KINDS);
const ABILITY_KIND_SET = new Set(ABILITY_KINDS);

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

function cmdSchema(kind) {
  if (!ALL_KINDS.includes(kind)) fail(`unsupported asset kind '${kind}' (supported: ${ALL_KINDS.join(", ")})`);
  const schema = VEHICLE_KIND_SET.has(kind)
    ? buildVehicleJsonSchema(kind)
    : ABILITY_KIND_SET.has(kind)
      ? buildAbilityJsonSchema(kind)
      : buildRegistryJsonSchema(kind);
  console.log(JSON.stringify(schema, null, 2));
}

function cmdExportSchemas(outDir) {
  const target = outDir ?? path.join(ENGINE_ROOT, "schema");
  fs.mkdirSync(target, { recursive: true });
  const written = [];
  for (const kind of ALL_KINDS) {
    const file = path.join(target, `${kind}.json`);
    const schema = VEHICLE_KIND_SET.has(kind)
      ? buildVehicleJsonSchema(kind)
      : ABILITY_KIND_SET.has(kind)
        ? buildAbilityJsonSchema(kind)
        : buildRegistryJsonSchema(kind);
    atomicWriteJson(file, schema);
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
  const result = VEHICLE_KIND_SET.has(kind)
    ? validateVehicleDocument(kind, document)
    : ABILITY_KIND_SET.has(kind)
      ? validateAbilityDocument(kind, document)
      : validateRegistryDocument(kind, document);
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
  const validation = VEHICLE_KIND_SET.has(kind)
    ? validateVehicleDocument(kind, document)
    : ABILITY_KIND_SET.has(kind)
      ? validateAbilityDocument(kind, document)
      : validateRegistryDocument(kind, document);
  if (!validation.valid) {
    for (const diagnostic of validation.errors) console.error(`- ${diagnostic}`);
    fail(`asset '${kind}/${name}' fails public-contract validation; nothing was written`);
  }
  const target = VEHICLE_KIND_SET.has(kind)
    ? path.join(engine, "Projects", project, "Content", "Vehicles", `${name}.json`)
    : ABILITY_KIND_SET.has(kind)
      ? path.join(engine, "Projects", project, "Content", "Abilities", `${name}.json`)
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
        "  kinds                     list supported registry + vehicle + ability kinds\n" +
        "  schema <kind>             print the JSON Schema (draft-07) for a kind\n" +
        "  export-schemas [outDir]   write <kind>.json for every kind\n" +
        "  validate <kind> <file>    validate an asset document (exit 1 on invalid)\n" +
        "  author --project <name> --kind <kind> --name <n> --file <doc.json> [--engine <root>] [--dry-run] [--update]\n"
      );
      process.exit(2);
  }
}

main();
