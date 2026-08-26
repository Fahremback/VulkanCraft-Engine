#!/usr/bin/env node
// mcp-conformance-gate.mjs — validates the MCP server's wire responses against
// the OFFICIAL MCP TypeScript SDK shapes, vendored at
// external/solutions/mcp-typescript-sdk/packages/core/src/schemas.ts
// (§7 mcp-typescript-sdk: "aproveitar protocolo, tipos e clientes/testes MCP").
//
// Dependency-free: the expected field sets are extracted from the vendored
// schemas.ts AT RUNTIME (single source of truth — no manual lists, §8 item 3),
// then the server's live responses are checked against them over stdio
// (newline framing). Any field rename in the SDK fails the extraction and the
// gate until the check is updated.
//
// Usage: node tools/portability/mcp-conformance-gate.mjs
import { spawn } from 'child_process';
import { readFileSync, existsSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const sdkSchemas = join(root, 'external', 'solutions', 'mcp-typescript-sdk', 'packages', 'core', 'src', 'schemas.ts');
if (!existsSync(sdkSchemas)) {
  console.error('[conformance] FAIL: vendored mcp-typescript-sdk schemas.ts not found');
  process.exit(1);
}
const schemaSrc = readFileSync(sdkSchemas, 'utf8');

// Extract the top-level field set of `export const <Name>Schema = ... ({ ... })`
// from the vendored SDK. Handles both `z.object({` and `X.extend({` definitions.
// Only 4-space-indented fields (depth 0) are captured; nested objects (8+
// spaces) are deliberately excluded. A rename/removal in the SDK makes the
// extraction fail (or the set shrink), failing the gate until the check is
// reconciled — the freshness invariant against the vendored SDK.
function extractObjectFields(schemaName) {
  const marker = `export const ${schemaName}Schema = `;
  const start = schemaSrc.indexOf(marker);
  if (start < 0) throw new Error(`${schemaName}Schema not found in vendored schemas.ts`);
  const brace = schemaSrc.indexOf('({', start);
  if (brace < 0) throw new Error(`${schemaName}Schema has no object literal`);
  const end = schemaSrc.indexOf('\n});', brace);
  if (end < 0) throw new Error(`${schemaName}Schema block not closed`);
  const fields = [];
  for (const line of schemaSrc.slice(brace, end).split('\n')) {
    const m = line.match(/^    ([A-Za-z_][A-Za-z0-9_]*):/);
    if (m) fields.push(m[1]);
  }
  return new Set(fields);
}

let initializeFields, capFields, toolFields, resourceFields, promptFields;
try {
  initializeFields = extractObjectFields('InitializeResult');
  capFields = extractObjectFields('ServerCapabilities');
  // Tool/Resource/Prompt inherit `name` (and optional `title`) from
  // BaseMetadataSchema via `...BaseMetadataSchema.shape` — union those in so
  // the strict key check accepts `name` on every listed tool/resource/prompt.
  const baseFields = extractObjectFields('BaseMetadata');
  toolFields = new Set([...extractObjectFields('Tool'), ...baseFields]);
  resourceFields = new Set([...extractObjectFields('Resource'), ...baseFields]);
  promptFields = new Set([...extractObjectFields('Prompt'), ...baseFields]);
} catch (e) {
  console.error(`[conformance] FAIL: ${e.message}`);
  process.exit(1);
}
for (const [label, set] of [['InitializeResult', initializeFields], ['ServerCapabilities', capFields], ['Tool', toolFields], ['Resource', resourceFields], ['Prompt', promptFields]]) {
  if (set.size === 0) { console.error(`[conformance] FAIL: ${label}Schema field extraction returned empty`); process.exit(1); }
  console.log(`[conformance] ${label}Schema fields (vendored SDK): ${[...set].sort().join(', ')}`);
}

const failures = [];
const check = (cond, msg) => { if (!cond) failures.push(msg); };
const isObj = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
const isStr = (v) => typeof v === 'string';

// ---- live server over stdio (newline framing) ----
const server = spawn(process.execPath, [join(root, 'tools', 'mcp-server', 'server.mjs')], {
  cwd: root, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true
});
let buf = '';
let seq = 0;
const pending = new Map();
server.stdout.setEncoding('utf8');
server.stdout.on('data', (d) => {
  buf += d;
  let idx;
  while ((idx = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, idx).trim();
    buf = buf.slice(idx + 1);
    if (!line) continue;
    const msg = JSON.parse(line);
    if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
  }
});
const request = (method, params = {}) => new Promise((resolve, reject) => {
  const id = ++seq;
  const timer = setTimeout(() => reject(new Error(`timeout waiting for ${method}`)), 15000);
  pending.set(id, (m) => { clearTimeout(timer); resolve(m); });
  server.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
});
// Notifications are fire-and-forget (no response, no id) — write without waiting.
const notify = (method, params = {}) => {
  server.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params }) + '\n');
};

(async () => {
  // ---- initialize ----
  const init = await request('initialize', { protocolVersion: '2025-03-26', capabilities: {}, clientInfo: { name: 'conformance', version: '1' } });
  check(!init.error, `initialize error: ${JSON.stringify(init.error)}`);
  const r = init.result;
  check(isStr(r.protocolVersion), 'initialize.result.protocolVersion is a string');
  check(initializeFields.has('protocolVersion') && initializeFields.has('capabilities') && initializeFields.has('serverInfo'), 'InitializeResult carries protocolVersion/capabilities/serverInfo');
  check(isObj(r.serverInfo) && isStr(r.serverInfo.name) && isStr(r.serverInfo.version), 'serverInfo has string name + version (ImplementationSchema)');
  check(isObj(r.capabilities), 'capabilities is an object');
  const extraCaps = Object.keys(r.capabilities).filter((k) => !capFields.has(k));
  check(extraCaps.length === 0, `capabilities keys outside ServerCapabilitiesSchema: ${extraCaps.join(', ')}`);
  if (r.capabilities.tools) check(typeof r.capabilities.tools.listChanged === 'boolean', 'capabilities.tools.listChanged is boolean');
  if (r.capabilities.resources) {
    check(typeof r.capabilities.resources.listChanged === 'boolean' && typeof r.capabilities.resources.subscribe === 'boolean', 'capabilities.resources.listChanged/subscribe are boolean');
  }
  if (r.capabilities.prompts) check(typeof r.capabilities.prompts.listChanged === 'boolean', 'capabilities.prompts.listChanged is boolean');
  notify('notifications/initialized');

  // ---- tools/list ----
  const toolsList = await request('tools/list', {});
  const tools = toolsList.result?.tools ?? [];
  check(tools.length > 0, 'tools/list returns tools');
  for (const t of tools) {
    check(isStr(t.name) && t.name.length > 0, `tool has string name (got ${JSON.stringify(t)})`);
    check(isStr(t.description), `tool '${t.name}' has string description`);
    check(isObj(t.inputSchema) && t.inputSchema.type === 'object', `tool '${t.name}' inputSchema.type === 'object' (MCP spec literal, ToolSchema)`);
    const extra = Object.keys(t).filter((k) => !toolFields.has(k));
    check(extra.length === 0, `tool '${t.name}' has keys outside ToolSchema: ${extra.join(', ')}`);
  }

  // ---- resources/list ----
  const resList = await request('resources/list', {});
  const resources = resList.result?.resources ?? [];
  for (const res of resources) {
    check(isStr(res.uri) && isStr(res.name), `resource has string uri + name (got ${JSON.stringify(res)})`);
    const extra = Object.keys(res).filter((k) => !resourceFields.has(k));
    check(extra.length === 0, `resource '${res.uri}' has keys outside ResourceSchema: ${extra.join(', ')}`);
  }

  // ---- prompts/list ----
  const promptsList = await request('prompts/list', {});
  const prompts = promptsList.result?.prompts ?? [];
  for (const p of prompts) {
    check(isStr(p.name) && p.name.length > 0, `prompt has string name (got ${JSON.stringify(p)})`);
    const extra = Object.keys(p).filter((k) => !promptFields.has(k));
    check(extra.length === 0, `prompt '${p.name}' has keys outside PromptSchema: ${extra.join(', ')}`);
  }

  server.stdin.end();
  if (failures.length > 0) {
    console.error('[conformance] FAIL:\n  - ' + failures.join('\n  - '));
    process.exit(1);
  }
  console.log(`[conformance] PASS — ${tools.length} tools, ${resources.length} resources, ${prompts.length} prompts conform to the vendored MCP TypeScript SDK (${server.exitCode === null ? 'server exited' : ''})`);
  process.exit(0);
})().catch((e) => { console.error(`[conformance] FAIL: ${e.message}`); process.exit(1); });
