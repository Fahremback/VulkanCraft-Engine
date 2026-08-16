#!/usr/bin/env node
// Registry asset CLI (FALTANTES item 10): expose the complete registry
// schemas and validate/author assets WITHOUT a running MCP server. Every
// command goes through the SAME factories the MCP server uses
// (game-authoring.mjs), so the CLI can never drift from what the mirror
// validation accepts — and the exported JSON Schemas are generated from the
// same REGISTRY_FIELD_SCHEMAS contracts.
//
// Usage:
//   node registry-cli.mjs kinds
//   node registry-cli.mjs schema <kind>
//   node registry-cli.mjs export-schemas [outDir]        (default: <engine>/schema/registry)
//   node registry-cli.mjs validate <kind> <file.json>
//   node registry-cli.mjs author --engine <root> --project <name> --kind <kind>
//                                --name <n> --file <doc.json> [--dry-run] [--update]
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  REGISTRY_KINDS,
  buildRegistryJsonSchema,
  validateRegistryDocument
} from "./game-authoring.mjs";

const SERVER_DIR = path.dirname(fileURLToPath(import.meta.url));
const ENGINE_ROOT = path.resolve(SERVER_DIR, "..", "..");

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
  console.log(REGISTRY_KINDS.join("\n"));
}

function cmdSchema(kind) {
  if (!REGISTRY_KINDS.includes(kind)) fail(`unsupported registry kind '${kind}' (supported: ${REGISTRY_KINDS.join(", ")})`);
  console.log(JSON.stringify(buildRegistryJsonSchema(kind), null, 2));
}

function cmdExportSchemas(outDir) {
  const target = outDir ?? path.join(ENGINE_ROOT, "schema", "registry");
  fs.mkdirSync(target, { recursive: true });
  const written = [];
  for (const kind of REGISTRY_KINDS) {
    const file = path.join(target, `${kind}.json`);
    atomicWriteJson(file, buildRegistryJsonSchema(kind));
    written.push(file);
  }
  for (const file of written) console.log(`wrote ${path.relative(ENGINE_ROOT, file).replaceAll(path.sep, "/")}`);
}

function cmdValidate(kind, file) {
  if (!REGISTRY_KINDS.includes(kind)) fail(`unsupported registry kind '${kind}' (supported: ${REGISTRY_KINDS.join(", ")})`);
  if (!file) fail("usage: validate <kind> <file.json>");
  let document;
  try {
    document = readJson(file);
  } catch (error) {
    fail(`cannot read '${file}': ${error.message}`);
  }
  const result = validateRegistryDocument(kind, document);
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
  if (!REGISTRY_KINDS.includes(kind)) fail(`unsupported registry kind '${kind}' (supported: ${REGISTRY_KINDS.join(", ")})`);
  let document;
  try {
    document = readJson(file);
  } catch (error) {
    fail(`cannot read '${file}': ${error.message}`);
  }
  const validation = validateRegistryDocument(kind, document);
  if (!validation.valid) {
    for (const diagnostic of validation.errors) console.error(`- ${diagnostic}`);
    fail(`asset '${kind}/${name}' fails public-contract validation; nothing was written`);
  }
  const target = path.join(engine, "Projects", project, "Content", "Registry", kind, `${name}.json`);
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
        "  kinds                     list supported registry kinds\n" +
        "  schema <kind>             print the JSON Schema (draft-07) for a kind\n" +
        "  export-schemas [outDir]   write <kind>.json for every kind\n" +
        "  validate <kind> <file>    validate an asset document (exit 1 on invalid)\n" +
        "  author --project <name> --kind <kind> --name <n> --file <doc.json> [--engine <root>] [--dry-run] [--update]\n"
      );
      process.exit(2);
  }
}

main();
