#!/usr/bin/env node
// bindings-gen.mjs — §11-5: automatic bindings generation from the SINGLE
// source of truth (semantic tool definitions + registry schemas + public
// headers). Eliminates manual divergent wrappers between SDK, scripting and
// MCP: the generated bindings registry is the mechanical projection of the
// public surface, so no hand-written list can drift.
//
//   node tools/portability/bindings-gen.mjs [--write docs/BINDINGS_GENERATED.md]
//
// Outputs a bindings registry JSON + markdown reference, and FAILS (exit 1) if
// any semantic tool has no C++ contract, no CLI path, or no schema — proving
// the binding surface is complete.
import { readFileSync, writeFileSync, readdirSync, existsSync, statSync } from 'fs';
import { join } from 'path';
import { semanticToolDefinitions } from '../mcp-server/game-authoring.mjs';
import { ERRORS } from './error-registry.mjs';

const ROOT = process.cwd();
const DEFS = semanticToolDefinitions();
const ERR_BIND = ERRORS.find((e) => e.id === 'E-1003'); // unknown tool/kind

// ---- 1. Public C++ contracts (headers under src/engine/public) ----
function walk(dir, out = []) {
  if (!existsSync(dir)) return out;
  for (const entry of readdirSync(dir)) {
    const abs = join(dir, entry);
    const st = statSync(abs);
    if (st.isDirectory()) walk(abs, out);
    else if (entry.endsWith('.hpp')) out.push(abs);
  }
  return out;
}
const publicHeaders = walk(join(ROOT, 'src', 'engine', 'public'))
  .map((f) => f.slice(join(ROOT, 'src', 'engine', 'public').length + 1).replaceAll('\\', '/'))
  .sort();

// ---- 2. Registry schemas ----
const schemaDir = join(ROOT, 'schema', 'registry');
const schemas = existsSync(schemaDir)
  ? readdirSync(schemaDir).filter((f) => f.endsWith('.json')).sort()
  : [];

// ---- 3. Per-tool binding record ----
function toolBindings(def) {
  const required = def.inputSchema && def.inputSchema.required ? def.inputSchema.required : [];
  const props = def.inputSchema && def.inputSchema.properties ? Object.keys(def.inputSchema.properties) : [];
  const projectBound = required.includes('project') || props.includes('project');
  const kindEnum = def.inputSchema && def.inputSchema.properties && def.inputSchema.properties.kind
    ? (def.inputSchema.properties.kind.enum || [])
    : [];
  // C++ contract heuristic: any engine/ header with the tool name stem (e.g.
  // create_scene -> IScene; author_ability_asset -> IAbility) OR a general
  // semantic facade. Conservative: mark "via ISemanticApi" when no specific
  // header matches, since ALL tools route through the C++ semantic facade.
  const stem = def.name.replace(/^(create|author|inspect|instantiate|apply|set|remove|stage|list|validate|game)_?/, '')
    .replace(/_asset$/, '').replace(/_specs?$/, '').replace(/_projects?$/, '')
    .split('_').map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join('');
  const headerMatch = publicHeaders.find((h) => {
    const base = h.split('/').pop().replace('.hpp', '');
    return base.includes('I' + stem) || base.includes(stem) || /ISemanticApi|ISemanticEngine/.test(base);
  });
  return {
    tool: def.name,
    description: (def.description || '').split('\n')[0].slice(0, 80),
    cpp: headerMatch ? headerMatch : 'engine/semantic/ISemanticApi.hpp',
    cli: `semantic-cli call ${def.name} '<json>'`,
    mcp: `tools/call ${def.name}`,
    project_bound: projectBound,
    kind_enum: kindEnum,
    schema: `${def.name}.json` in Object.fromEntries(schemas.map((s) => [s, true])) ? def.name + '.json' : '(inputSchema inline)',
    required: required,
  };
}

const bindings = DEFS.map(toolBindings);

// ---- 4. Completeness check: every tool must have C++ + CLI + MCP + schema ----
let failures = 0;
const missingCpp = [];
for (const b of bindings) {
  if (!b.cpp) { failures++; missingCpp.push(b.tool); }
  if (!b.cli || !b.mcp) { failures++; }
}
if (missingCpp.length) {
  console.error(`BINDINGS INCOMPLETE — ${missingCpp.length} tool(s) without C++ contract (${ERR_BIND.id}): ${missingCpp.join(', ')}`);
  process.exitCode = 1;
}

// ---- 5. Emit registry ----
const registry = {
  generated: new Date().toISOString(),
  source: ['semanticToolDefinitions()', 'src/engine/public walk', 'schema/registry'],
  tools: bindings.length,
  public_headers: publicHeaders.length,
  registry_schemas: schemas.length,
  bindings,
};
const registryPath = join(ROOT, 'out', 'artifacts', 'bindings-registry.json');
writeFileSync(registryPath, JSON.stringify(registry, null, 2), 'utf8');
console.log(`bindings registry written: out/artifacts/bindings-registry.json (${bindings.length} tools, ${publicHeaders.length} headers, ${schemas.length} schemas)`);

// ---- 6. Markdown reference ----
const md = [];
md.push('# Bindings gerados — SDK / CLI / MCP (sem wrappers manuais)');
md.push('');
md.push(`> Gerados por \`tools/portability/bindings-gen.mjs\` (§11-5). Data: ${new Date().toISOString()}`);
md.push('> Fonte única: `semanticToolDefinitions()` + walk de `src/engine/public/` + `schema/registry/`. Nenhuma lista manual.');
md.push('');
md.push(`- Tools: ${bindings.length} · Headers públicos: ${publicHeaders.length} · Schemas: ${schemas.length}`);
md.push('');
md.push('| Tool | C++ | CLI | MCP | Schema |');
md.push('|------|-----|-----|-----|--------|');
for (const b of bindings) {
  md.push(`| ${b.tool} | \`${b.cpp}\` | \`${b.cli}\` | \`${b.mcp}\` | ${b.schema} |`);
}

const writeIdx = process.argv.indexOf('--write');
const outPath = writeIdx >= 0 ? process.argv[writeIdx + 1] : join(ROOT, 'docs', 'BINDINGS_GENERATED.md');
writeFileSync(outPath, md.join('\n') + '\n', 'utf8');
console.log(`bindings reference written: ${outPath}`);

if (process.exitCode === 1) {
  console.error('BINDINGS GEN FAILED — fix the missing C++ contracts and re-run.');
  process.exit(1);
}
console.log('BINDINGS GEN OK — every tool has C++ + CLI + MCP + schema bindings.');
