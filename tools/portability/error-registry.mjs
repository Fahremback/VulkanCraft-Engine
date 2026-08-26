#!/usr/bin/env node
// error-registry.mjs — Central stable error registry for SDK/MCP/CLI (§11-21).
// Single source of truth for error codes: E-XXXX stable IDs, short messages,
// suggested fixes. Verifies each code is actually emitted somewhere in the
// public surface (src/engine/public, tools/mcp-server) and generates
// docs/ERROR_REGISTRY.md. Exit 0 = all codes present; 1 = orphan/missing code.
import { readFileSync, readdirSync, statSync, writeFileSync, existsSync } from 'fs';
import { join, sep } from 'path';

const ROOT = process.cwd();

// ---- Stable error registry (single source of truth) ----
// Fields: id (stable), domain, message (short), fix (actionable), surface (SDK/MCP/CLI)
const ERRORS = [
  { id: 'E-1001', domain: 'sdk', message: 'Invalid argument value', fix: 'Check the parameter contract; most tools validate ranges/enums and refuse with the offending key.', surface: 'SDK/MCP/CLI' },
  { id: 'E-1002', domain: 'sdk', message: 'Operation refused (validation failed)', fix: 'Run with dry_run:true first; the refusal message names the failing key.', surface: 'SDK/MCP/CLI' },
  { id: 'E-1003', domain: 'sdk', message: 'Unknown tool/kind/type', fix: 'Call game_capabilities or capability-matrix to list valid tools/kinds.', surface: 'MCP/CLI' },
  { id: 'E-1004', domain: 'sdk', message: 'Project not found or not initialized', fix: 'Create the project with create_game_project or check the path.', surface: 'SDK/MCP/CLI' },
  { id: 'E-1005', domain: 'sdk', message: 'Document/asset already exists', fix: 'Use update:true to replace, or choose a different name.', surface: 'MCP/CLI' },
  { id: 'E-1006', domain: 'sdk', message: 'Serialization/parse failure', fix: 'Validate the JSON against schema/registry/*.json (draft-07) before calling.', surface: 'SDK/MCP/CLI' },
  { id: 'E-2001', domain: 'mcp', message: 'Unsupported protocol version', fix: 'Negotiate with initialize: pass a supportedProtocolVersions value.', surface: 'MCP' },
  { id: 'E-2002', domain: 'mcp', message: 'Invalid JSON-RPC request', fix: 'Use Content-Length framing or newline-delimited JSON per README.', surface: 'MCP' },
  { id: 'E-2003', domain: 'mcp', message: 'Method not found', fix: 'List available tools via tools/list before calling.', surface: 'MCP' },
  { id: 'E-2004', domain: 'mcp', message: 'Path outside allowed roots', fix: 'Only write under ENGINE_ROOT; BLOCKED_WRITE_ROOTS are protected.', surface: 'MCP' },
  { id: 'E-2005', domain: 'mcp', message: 'Rate limit exceeded', fix: 'Slow down; token bucket is 200/s burst 400 per process.', surface: 'MCP' },
  { id: 'E-2006', domain: 'mcp', message: 'Payload too large', fix: 'Split the request or reduce asset payload below MAX_TEXT_BYTES.', surface: 'MCP' },
  { id: 'E-3001', domain: 'cli', message: 'CLI usage error (bad flags/args)', fix: 'Run with --help; semantic-cli exits 3 on driver errors.', surface: 'CLI' },
  { id: 'E-3002', domain: 'cli', message: 'Operation failed (isError result)', fix: 'Inspect the structured result; semantic-cli exits 2 on isError.', surface: 'CLI' },
  { id: 'E-3003', domain: 'cli', message: 'Registry export/schema error', fix: 'Re-run registry-cli export-schemas; committed schemas must equal fresh regeneration.', surface: 'CLI' },
  { id: 'E-4001', domain: 'build', message: 'Build failed or invalid configuration', fix: 'Check build job log via build_status/jobArtifacts.log; re-run with a valid preset.', surface: 'MCP/CLI' },
  { id: 'E-4002', domain: 'build', message: 'Package failed (all-or-nothing)', fix: 'Ensure the executable exists and Content/ is valid before package_game.', surface: 'MCP/CLI' },
  { id: 'E-5001', domain: 'sdk', message: 'SDK install/consume misconfiguration', fix: 'Use find_package(vulkan_craft_sdk CONFIG); check moved-prefix gate for relocatable install.', surface: 'SDK/CLI' },
];

// ---- Verification: each code must appear in the public surface ----
const SEARCH_DIRS = [
  join(ROOT, 'src', 'engine', 'public'),
  join(ROOT, 'tools', 'mcp-server'),
  join(ROOT, 'tools', 'sdk'),
  join(ROOT, 'tools', 'portability'),
];

const EXTENSIONS = new Set(['.hpp', '.h', '.cpp', '.mjs', '.js', '.md', '.json']);

export { ERRORS };

export function errorById(id) {
  return ERRORS.find((e) => e.id === id) || null;
}

function walk(dir, out = []) {
  if (!existsSync(dir)) return out;
  for (const entry of readdirSync(dir)) {
    const abs = join(dir, entry);
    let st;
    try { st = statSync(abs); } catch { continue; }
    if (st.isDirectory()) {
      if (entry === 'node_modules' || entry === '.git') continue;
      walk(abs, out);
    } else if (EXTENSIONS.has(entry.slice(entry.lastIndexOf('.')))) {
      out.push(abs);
    }
  }
  return out;
}

function codeMentioned(id) {
  const files = walk(join(ROOT, 'src', 'engine', 'public'))
    .concat(walk(join(ROOT, 'tools', 'mcp-server')))
    .concat(walk(join(ROOT, 'tools', 'sdk')))
    .concat(walk(join(ROOT, 'tools', 'portability')));
  for (const file of files) {
    if (file.endsWith('error-registry.mjs')) continue; // never self-verify against the registry source
    try {
      const text = readFileSync(file, 'utf8');
      // code itself OR a shared constant referencing it (e.g. ERROR_E_1001 = "E-1001")
      if (text.includes(id)) return true;
    } catch { /* skip */ }
  }
  return false;
}

// ---- Main (only when run directly; importable as a module) ----
const isMain = process.argv[1] && process.argv[1].replaceAll('\\', '/').endsWith('error-registry.mjs');

function runMain() {
  const results = ERRORS.map((e) => ({ ...e, found: codeMentioned(e.id) }));
  const missing = results.filter((r) => !r.found);

  // ---- Generate docs/ERROR_REGISTRY.md ----
  const lines = [];
  lines.push('# Central Error Registry — SDK / MCP / CLI');
  lines.push('');
  lines.push(`> Gerado automaticamente por \`tools/portability/error-registry.mjs\` (§11-21). Data: ${new Date().toISOString()}`);
  lines.push('> Códigos estáveis E-XXXX. Nunca edite este arquivo à mão — edite o registry no script e regenere.');
  lines.push('');
  lines.push('## Registry');
  lines.push('');
  lines.push('| ID | Domain | Message | Suggested fix | Surface |');
  lines.push('|----|--------|---------|---------------|---------|');
  for (const r of results) {
    lines.push(`| ${r.id} | ${r.domain} | ${r.message} | ${r.fix} | ${r.surface} |`);
  }
  lines.push('');
  lines.push('## Verificação');
  lines.push('');
  lines.push(`- Códigos registrados: ${results.length}`);
  lines.push(`- Códigos com referência real no código público: ${results.length - missing.length}`);
  lines.push(`- Códigos órfãos (sem referência): ${missing.length} ${missing.length ? '(' + missing.map((m) => m.id).join(', ') + ')' : '✅'}`);
  lines.push('');
  lines.push('## Regras');
  lines.push('');
  lines.push('- IDs são estáveis e nunca reutilizados; deprecation preserva o ID.');
  lines.push('- MCP `isError` + `semantic-cli` exit 2 e `registry-cli` exit 2 mapeiam para E-3002/E-3003.');
  lines.push('- Novos códigos: adicionar ao registry E ANTES de emitir no código; o gate falha com órfãos.');

  const docPath = join(ROOT, 'docs', 'ERROR_REGISTRY.md');
  writeFileSync(docPath, lines.join('\n') + '\n', 'utf8');
  console.log(`docs/ERROR_REGISTRY.md written (${results.length} codes, ${missing.length} orphaned)`);

  if (missing.length) {
    console.error('ORPHAN CODES (registered but never emitted): ' + missing.map((m) => m.id).join(', '));
    console.error('Fix: emit these codes in the surface or remove them from the registry.');
    process.exit(1);
  }
  console.log('ERROR REGISTRY OK — every code has a real reference in the public surface.');
  process.exit(0);
}

if (isMain) runMain();
