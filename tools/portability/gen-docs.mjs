#!/usr/bin/env node
// gen-docs.mjs — §11-27: generate documentation automatically from the REAL
// public surface (no manual lists): public headers, semantic tool definitions,
// registry schemas, error registry. Output: docs/REFERENCE_GENERATED.md.
// Exit 0 = generated; 1 = a source could not be read.
import { readFileSync, readdirSync, writeFileSync, existsSync, statSync } from 'fs';
import { join } from 'path';
import { semanticToolDefinitions } from '../mcp-server/game-authoring.mjs';
import { ERRORS } from './error-registry.mjs';

const ROOT = process.cwd();
const PUBLIC = join(ROOT, 'src', 'engine', 'public');
const SCHEMA_DIR = join(ROOT, 'schema', 'registry');
const OUT = join(ROOT, 'docs', 'REFERENCE_GENERATED.md');
const COOKBOOK_OUT = join(ROOT, 'docs', 'COOKBOOK_GENERATED.md');

// Exemplo sintético por tipo de propriedade do schema (mesma fonte única).
function exampleFor(type) {
  switch (type) {
    case 'string': return '"sample"';
    case 'integer': return '1';
    case 'number': return '1.0';
    case 'boolean': return 'true';
    case 'array': return '[]';
    case 'object': return '{}';
    default: return '"sample"';
  }
}

function exampleArgs(properties) {
  const parts = [];
  for (const [name, prop] of Object.entries(properties || {})) {
    parts.push(`${JSON.stringify(name)}: ${exampleFor(prop.type)}`);
  }
  return `{ ${parts.join(', ')} }`;
}

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

const lines = [];
lines.push('# Reference gerada automaticamente — contratos públicos');
lines.push('');
lines.push('> Gerada por `tools/portability/gen-docs.mjs` (§11-27). Sem data/hora: a saída é byte-estável (gate anti-drift no smoke).');
lines.push('> Fonte única: walk de `src/engine/public/`, `semanticToolDefinitions()`, `schema/registry/`, `error-registry.mjs`.');
lines.push('> Nunca edite este arquivo à mão — regenere.');

// 1. Public headers
const headers = walk(PUBLIC).map((f) => f.slice(PUBLIC.length + 1).replaceAll('\\', '/')).sort();
lines.push('');
lines.push('## 1. Headers públicos (C++)');
lines.push('');
lines.push(`Total: ${headers.length}`);
lines.push('');
lines.push('```');
for (const h of headers) lines.push(h);
lines.push('```');

// 2. Semantic tools
const defs = semanticToolDefinitions();
lines.push('');
lines.push('## 2. Tools semânticas (MCP/CLI — mesmas factories)');
lines.push('');
lines.push(`Total: ${defs.length}`);
lines.push('');
lines.push('| Tool | Description |');
lines.push('|------|-------------|');
for (const d of defs) {
  const desc = (d.description || '').split('\n')[0];
  lines.push(`| ${d.name} | ${desc} |`);
}

// 3. Registry schemas
lines.push('');
lines.push('## 3. Schemas de registry');
lines.push('');
if (existsSync(SCHEMA_DIR)) {
  const schemas = readdirSync(SCHEMA_DIR).filter((f) => f.endsWith('.json')).sort();
  lines.push(`Total: ${schemas.length}`);
  lines.push('');
  lines.push('```');
  for (const s of schemas) lines.push(s);
  lines.push('```');
} else {
  lines.push('(schema/registry não encontrado)');
}

// 4. Error registry
lines.push('');
lines.push('## 4. Códigos de erro estáveis');
lines.push('');
lines.push('| ID | Domain | Message | Surface |');
lines.push('|----|--------|---------|---------|');
for (const e of ERRORS) {
  lines.push(`| ${e.id} | ${e.domain} | ${e.message} | ${e.surface} |`);
}

writeFileSync(OUT, lines.join('\n') + '\n', 'utf8');

// 5. Cookbook gerado — mesmas factories, zero listas manuais: um exemplo por
// tool semântica (JSON-RPC style) + domínios C++ por diretório.
const cook = [];
cook.push('# Cookbook gerado automaticamente — exemplos por tool e domínio');
cook.push('');
cook.push('> Gerada por `tools/portability/gen-docs.mjs` (§11-27). Sem data/hora: a saída é byte-estável (gate anti-drift no smoke).');
cook.push('> Fonte única: `semanticToolDefinitions()` + walk de `src/engine/public/` — NUNCA edite à mão, regenere.');
cook.push('');
cook.push('## 1. Exemplos por tool semântica (MCP `tools/call` / CLI `semantic-cli`)');
cook.push('');
for (const d of defs) {
  const props = (d.inputSchema && d.inputSchema.properties) || {};
  cook.push(`### ${d.name}`);
  cook.push('');
  const desc = (d.description || '').split('\n')[0];
  cook.push(`${desc}`);
  cook.push('');
  if (Object.keys(props).length > 0) {
    cook.push('```json');
    cook.push(`{ "jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": { "name": "${d.name}", "arguments": ${exampleArgs(props)} } }`);
    cook.push('```');
  } else {
    cook.push('```json');
    cook.push(`{ "jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": { "name": "${d.name}", "arguments": {} } }`);
    cook.push('```');
  }
  cook.push('');
}

// 2. Domínios C++ por diretório (mesma fonte do inventário).
const byDomain = {};
for (const h of headers) {
  const domain = h.split('/')[1] || '(raiz)';
  (byDomain[domain] ||= []).push(h);
}
cook.push('## 2. Domínios C++ públicos (por diretório)');
cook.push('');
for (const [domain, files] of Object.entries(byDomain).sort()) {
  cook.push(`### ${domain}`);
  cook.push('');
  cook.push('```');
  for (const f of files) cook.push(f);
  cook.push('```');
  cook.push('');
}

writeFileSync(COOKBOOK_OUT, cook.join('\n') + '\n', 'utf8');
console.log(`docs/REFERENCE_GENERATED.md written (${headers.length} headers, ${defs.length} tools, ${ERRORS.length} error codes)`);
console.log(`docs/COOKBOOK_GENERATED.md written (${defs.length} tool examples, ${Object.keys(byDomain).length} domains)`);
