#!/usr/bin/env node

// CLI driver para o servidor MCP VulkanCraft Engine — permite chamar qualquer
// ferramenta sem um cliente MCP gráfico:
//
//   node mcp-call.mjs engine_overview
//   node mcp-call.mjs search_code '{"query": "add_executable", "path": "."}'
//   node mcp-call.mjs run_game '{"exe": "VulkanEngineServer", "seconds": 8}'
//
// O servidor é iniciado, inicializado e encerrado a cada chamada (stdio).
// Exit code: 0 = sucesso, 2 = a ferramenta retornou isError, 3 = erro de driver.

import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const directory = path.dirname(fileURLToPath(import.meta.url));
const [toolName, rawArgs] = process.argv.slice(2);
if (!toolName) {
  console.error("uso: node mcp-call.mjs <toolName> ['<json-args>']");
  process.exit(3);
}
let args = {};
if (rawArgs !== undefined) {
  try {
    args = JSON.parse(rawArgs);
  } catch (error) {
    console.error(`args JSON inválido: ${error.message}`);
    process.exit(3);
  }
}

const child = spawn(process.execPath, [path.join(directory, "server.mjs")], {
  cwd: directory,
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true
});

let buffer = "";
let stderr = "";
let nextId = 1;
const pending = new Map();

child.stderr.setEncoding("utf8");
child.stderr.on("data", (chunk) => { stderr += chunk; });
child.stdout.setEncoding("utf8");
child.stdout.on("data", (chunk) => {
  buffer += chunk;
  let newline;
  while ((newline = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, newline).trim();
    buffer = buffer.slice(newline + 1);
    if (!line) continue;
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      continue;
    }
    const resolve = pending.get(message.id);
    if (resolve) {
      pending.delete(message.id);
      resolve(message);
    }
  }
});

function request(method, params = {}) {
  const id = nextId++;
  return new Promise((resolve) => {
    pending.set(id, resolve);
    child.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`);
  });
}

const timeout = setTimeout(() => {
  console.error(`timeout aguardando o servidor MCP; stderr:\n${stderr}`);
  child.kill();
  process.exit(3);
}, 30000);

try {
  const initialized = await request("initialize", { protocolVersion: "2025-03-26", capabilities: {} });
  if (!initialized.result?.serverInfo) {
    console.error(JSON.stringify(initialized, null, 2));
    process.exit(3);
  }
  const response = await request("tools/call", { name: toolName, arguments: args });
  if (response.error) {
    console.error(JSON.stringify(response.error, null, 2));
    process.exit(3);
  }
  for (const part of response.result?.content ?? []) {
    if (part.type === "text") process.stdout.write(`${part.text}\n`);
  }
  process.exit(response.result?.isError ? 2 : 0);
} finally {
  clearTimeout(timeout);
  child.stdin.end();
  child.kill();
}
