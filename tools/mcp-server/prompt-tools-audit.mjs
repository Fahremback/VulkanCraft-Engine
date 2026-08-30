#!/usr/bin/env node
// prompt-tools-audit.mjs
//
// Agente 3 (fechamento_solidacao) — codifies the Rodada 3 finding that the 12
// MCP authoring prompts are NOT ghost tool references. A prompt is valid only if
// every tool it tells the LLM to *call* ("Call \`name\`") exists in the exact
// catalog the server serves via tools/list (mcpToolCatalog(), same as runtime).
//
// Backticks also wrap JSON *field names* in the bullet lists (e.g. `hardness`,
// `biomes`, `color`). To avoid false positives the audit only treats a backtick
// token as a tool reference when it is a real invocation: a snake_case/camel
// token that (a) follows the word "call" (case-insensitive) as an instruction,
// or (b) resolves to a registered tool / prompt name. Reference to a field-name
// token is allowed; an unknown token in *call* position FAILS.
//
// Fails WITHOUT a fix if any prompt says "Call \`not_a_real_tool\`"; passes WITH.
import assert from "node:assert/strict";
import process from "node:process";

process.env.VC_MCP_AUDIT_ONLY = "1";
const { PROMPTS, renderPrompt, mcpToolCatalog } = await import("./server.mjs");

const toolNames = new Set(mcpToolCatalog().map((t) => t.name));
const promptNames = new Set(PROMPTS.map((p) => p.name));

console.log(`[prompt-tools-audit] tools=${toolNames.size} prompts=${promptNames.size}`);

function sampleArgs(prompt) {
  const args = {};
  for (const a of prompt.arguments ?? []) args[a.name] = `sample_${prompt.name}`;
  return args;
}

const isIdent = (s) => /^[A-Za-z][A-Za-z0-9_]*$/.test(s);
const CALL_RE = /\bcall\s+`([A-Za-z][A-Za-z0-9_]*)`/gi;
const TOKEN_RE = /`([A-Za-z][A-Za-z0-9_]*)`/g;

let failures = 0;
let totalCalls = 0;
for (const prompt of PROMPTS) {
  const rendered = renderPrompt(prompt.name, sampleArgs(prompt));
  const text = rendered.messages.map((m) => m.content.text ?? "").join("\n");

  // Tokens explicitly instructed to be CALLED: `Call name` (the real tool-use
  // signal in every template).
  const called = [];
  let cm;
  CALL_RE.lastIndex = 0;
  while ((cm = CALL_RE.exec(text)) !== null) called.push(cm[1]);

  // Tokens that resolve to a registered tool or a prompt aliasing one (e.g.
  // create_material is both a prompt and a tool; "and `read_file`" is tool-adjacent
  // but not behind "call", yet read_file is a real tool). These are legitimate.
  const resolvable = [];
  let tm;
  TOKEN_RE.lastIndex = 0;
  while ((tm = TOKEN_RE.exec(text)) !== null) {
    const tok = tm[1];
    if (toolNames.has(tok) || promptNames.has(tok)) resolvable.push(tok);
  }
  const resolvedSet = new Set([...called, ...resolvable]);

  // Unknown = a call-position token that is NOT a registered tool AND not itself a
  // prompt alias. Field names are never in call position, so they never land here.
  const unknown = [...new Set(called)].filter((t) => !toolNames.has(t) && !promptNames.has(t));
  totalCalls += called.length;

  if (unknown.length) {
    failures += 1;
    console.error(`  ✗ prompt '${prompt.name}': unknown called tools ${unknown.join(", ")}`);
  } else {
    console.log(`  ✓ prompt '${prompt.name}' (${called.length} call refs, ${resolvedSet.size} resolvable, all resolved)`);
  }
}

console.log(`[prompt-tools-audit] total tool-call refs=${totalCalls}`);
assert.equal(failures, 0, `${failures} prompt(s) tell the LLM to call tools that are not registered`);
console.log("[prompt-tools-audit] ALL PROMPTS REFERENCE ONLY REAL CALLABLE TOOLS: PASS");
process.exit(0);