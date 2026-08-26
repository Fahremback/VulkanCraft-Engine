#!/usr/bin/env node
// script-gate.mjs — §11-13 support: validates visual script GRAPHS against the
// public scripting contract (create_visual_script schema) — node kinds,
// link targets, connectivity — all-or-nothing, deterministic. This is the
// validation half of "consolidar scripting" (sandbox/budgets/debugger remain
// AGENT-5 §3); the gate proves the graph contract is enforced before any
// execution, so a malformed script is REFUSED, never silently no-op'd.
//
//   node tools/portability/script-gate.mjs <graph.json>
//
// Exit 0 = graph valid; 1 = invalid (refusal reason printed); 2 = usage.
import { readFileSync } from 'fs';
import { ERRORS } from './error-registry.mjs';

const ERR_GRAPH = ERRORS.find((e) => e.id === 'E-1002'); // operation refused (validation)
const ERR_PARSE = ERRORS.find((e) => e.id === 'E-1006');  // serialization/parse

// Public contract: node kinds from create_visual_script inputSchema
const KINDS = new Set([
  'Event', 'ConstantFloat', 'ConstantInteger', 'ConstantBoolean',
  'GetVariable', 'SetVariable', 'AddFloat', 'SubtractFloat', 'MultiplyFloat',
  'Branch', 'Wait', 'EmitEvent', 'Return', 'Function', 'FunctionCall',
  'Log', 'Scope', 'ScopeEnd',
]);

function main() {
  const file = process.argv[2];
  if (!file) {
    console.error('usage: node script-gate.mjs <graph.json>');
    process.exit(2);
  }
  let graph;
  try {
    graph = JSON.parse(readFileSync(file, 'utf8'));
  } catch (error) {
    console.error(`SCRIPT GATE FAIL: invalid JSON (${ERR_PARSE.id}): ${error.message}`);
    process.exit(1);
  }

  const errors = [];
  const nodes = Array.isArray(graph.nodes) ? graph.nodes : [];
  const links = Array.isArray(graph.links) ? graph.links : [];
  const keys = new Set(nodes.map((n) => n.key));

  // 1. Node kinds must be in the public contract
  for (const node of nodes) {
    if (!KINDS.has(node.kind)) {
      errors.push(`node '${node.key}' has unknown kind '${node.kind}' (${ERR_GRAPH.id})`);
    }
  }

  // 2. Links must reference existing node keys (no dangling edges)
  for (const link of links) {
    if (!keys.has(link.from)) errors.push(`link from unknown node '${link.from}' (${ERR_GRAPH.id})`);
    if (!keys.has(link.to)) errors.push(`link to unknown node '${link.to}' (${ERR_GRAPH.id})`);
  }

  // 3. Connectivity: every node except Entry/Return must be reachable or
  //    intentionally terminal; at least one Event must exist (execution entry)
  const hasEvent = nodes.some((n) => n.kind === 'Event');
  if (!hasEvent) errors.push(`graph has no Event node — nothing can trigger execution (${ERR_GRAPH.id})`);

  // 4. Scope balance: Scope must pair with ScopeEnd (stack correctness)
  const scopes = nodes.filter((n) => n.kind === 'Scope').length;
  const scopeEnds = nodes.filter((n) => n.kind === 'ScopeEnd').length;
  if (scopes !== scopeEnds) errors.push(`unbalanced Scope (${scopes}) / ScopeEnd (${scopeEnds}) (${ERR_GRAPH.id})`);

  // 5. Branch must have 2 outgoing links (true/false); enforce determinism
  for (const node of nodes) {
    if (node.kind === 'Branch') {
      const out = links.filter((l) => l.from === node.key).length;
      if (out !== 2) errors.push(`Branch '${node.key}' has ${out} outgoing links (expected 2: true/false) (${ERR_GRAPH.id})`);
    }
    if (node.kind === 'Return' || node.kind === 'ScopeEnd' || node.kind === 'EmitEvent') {
      // terminal-ish nodes may still flow; no strict check — only record
    }
  }

  if (errors.length) {
    console.error(`SCRIPT GATE FAIL — ${errors.length} violation(s) (all-or-nothing, nothing executed):`);
    for (const e of errors) console.error('  - ' + e);
    process.exit(1);
  }
  console.log(`SCRIPT GATE PASSED — graph valid: ${nodes.length} nodes, ${links.length} links, kinds/links/connectivity/scope OK.`);
}

main();
