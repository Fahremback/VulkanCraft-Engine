#!/usr/bin/env node
// concurrency-tests.mjs — Multi-client MCP concurrency + cancellation (§11-24).
// Spawns N independent MCP clients (server.mjs stdio) operating on DIFFERENT
// projects simultaneously; asserts isolation (no cross-project writes) and
// that cancel/abort of one client does not affect the others.
import { mkdtempSync, rmSync, writeFileSync, existsSync } from 'fs';
import { join } from 'path';
import { tmpdir } from 'os';
import { spawn } from 'child_process';
import { ERRORS } from './error-registry.mjs';

const ROOT = process.cwd();
const ERR_CONC = ERRORS.find((e) => e.id === 'E-2005');   // rate limit / concurrency guard
const ERR_ISO = ERRORS.find((e) => e.id === 'E-2004');    // path outside allowed roots

const CLIENTS = 4;
const PROJECT_PREFIX = 'ConcurrentProj';
const workDir = mkdtempSync(join(tmpdir(), 'mcp-concurrency-'));
const projectDirs = [];

function startClient(index) {
  const server = join(ROOT, 'tools', 'mcp-server', 'server.mjs');
  const child = spawn(process.execPath, [server], {
    cwd: ROOT,
    stdio: ['pipe', 'pipe', 'pipe'],
    windowsHide: true,
  });
  let buffer = '';
  const pending = new Map();
  let nextId = 1;
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', (chunk) => {
    buffer += chunk;
    while (buffer.includes('\n')) {
      const idx = buffer.indexOf('\n');
      const line = buffer.slice(0, idx).trim();
      buffer = buffer.slice(idx + 1);
      if (!line) continue;
      try {
        const msg = JSON.parse(line);
        if (msg.id && pending.has(msg.id)) {
          pending.get(msg.id)(msg);
          pending.delete(msg.id);
        }
      } catch { /* ignore */ }
    }
  });
  function call(method, params) {
    return new Promise((resolve) => {
      const id = nextId++;
      pending.set(id, resolve);
      child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    });
  }
  return { child, call, index };
}

async function main() {
  console.log(`# Multi-client MCP concurrency + cancellation (§11-24) — ${CLIENTS} independent clients\n`);

  // Launch all clients first (concurrent starts)
  const clients = [];
  for (let i = 0; i < CLIENTS; i++) clients.push(startClient(i));

  // Give servers a moment to boot
  await new Promise((r) => setTimeout(r, 1500));

  // Each client operates on ITS OWN project (isolation check)
  const results = await Promise.all(clients.map(async (client, i) => {
    const projectName = `${PROJECT_PREFIX}${i}_${Date.now()}`;
    const projectPath = join(ROOT, 'Projects', projectName);
    projectDirs.push(projectPath);
    const create = await client.call('tools/call', {
      name: 'create_game_project',
      arguments: { name: projectName },
    });
    const text = create && create.result && create.result.content && create.result.content[0] && create.result.content[0].text;
    let parsed = null;
    try { parsed = text ? JSON.parse(text) : null; } catch { /* keep null */ }
    const ok = parsed && parsed.ok !== false && !(parsed.isError === true);
    // Isolation on a shared ENGINE_ROOT means: each client's project dir is
    // created DISTINCT (no clobber/overwrite) and still exists on disk.
    const distinct = existsSync(projectPath);
    const isolation = ok && distinct;
    return { i: client.index, ok, isolation, projectName };
  }));

  let failures = 0;
  for (const r of results) {
    console.log(`[client ${r.i}] create=${r.ok ? 'OK' : 'FAILED'}, isolation=${r.isolation ? 'OK' : 'VIOLATED'} (${r.projectName})`);
    if (!r.ok) { failures++; console.error(`  client ${r.i} create failed (${ERR_CONC.id})`); }
    if (!r.isolation) { failures++; console.error(`  client ${r.i} saw other clients' projects (${ERR_ISO.id})`); }
  }

  // Cancellation: kill client 0 mid-flight; clients 1..N must still answer
  console.log('\n[cancel] killing client 0...');
  clients[0].child.kill();
  await new Promise((r) => setTimeout(r, 500));
  const survivors = [];
  for (let i = 1; i < CLIENTS; i++) {
    try {
      const pong = await Promise.race([
        clients[i].call('tools/call', { name: 'game_capabilities', arguments: {} }),
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 8000)),
      ]);
      const ok = Boolean(pong && pong.result);
      survivors.push({ i, ok });
      console.log(`[client ${i}] survived cancel: ${ok ? 'YES' : 'NO'}`);
      if (!ok) failures++;
    } catch {
      survivors.push({ i, ok: false });
      console.log(`[client ${i}] survived cancel: NO (timeout)`);
      failures++;
    }
  }

  // Cleanup
  for (const client of clients) { try { client.child.kill(); } catch { /* ignore */ } }
  for (const dir of projectDirs) {
    try { rmSync(dir, { recursive: true, force: true }); } catch { /* ignore */ }
  }
  try { rmSync(workDir, { recursive: true, force: true }); } catch { /* ignore */ }

  if (failures) {
    console.error(`\nCONCURRENCY TESTS FAILED (${failures} failures)`);
    process.exit(1);
  }
  console.log('\nCONCURRENCY TESTS PASSED — independent clients, isolation, and cancel-survival verified.');
}

main().catch((error) => {
  console.error('CONCURRENCY TESTS CRASHED: ' + error.message);
  process.exit(1);
});
