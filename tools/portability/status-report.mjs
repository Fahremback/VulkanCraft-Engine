#!/usr/bin/env node
// status-report.mjs — Generate an honest summary from the five current plans.
//
// Plan H (agente4): diferenciar tarefa implementada, integrada e certificada;
// detectar qualquer ID de bug e qualquer estado diferente de RESOLVIDO (não só
// linhas `BUG-*`); sem contagens/estados "ALL PASSED" hardcoded.
//
// Convenções de estado lidas dos planos (sem inventar marcadores novos):
//   [x]                           -> implementada  (código/trabalho existe)
//   [x] ... build/validação A5... -> implementada + aguardando certificação A5
//   [x] ... ✔ / certificada ...   -> certificada   (validação de entrega)
//   [ ]                           -> pendente
import { readFileSync } from 'fs';
import { join } from 'path';

const agentDirs = [
    'agentes/agente1_runtime_render',
    'agentes/agente2_world_gameplay',
    'agentes/agente3_network_server',
    'agentes/agente4_editor_sdk_mcp',
    'agentes/agente5_integracao_entrega'
];

// A single item can carry several annotations; counts are NOT mutually exclusive:
// implemented = [x]; awaitingA5 = [x] que ainda depende da validação do A5;
// certified = [x] com marca explícita de certificação.
const AWAIT_A5_RE = /build\s*\/\s*valida[çc][aã]o\s+A5|valida[çc][aã]o(?:\s+do)?\s+A5|A5\s+compila|pendente.*A5|A5.*pendente/i;
const CERTIFIED_RE = /✔|✅|certificad[ao]/i;

function countItems(content) {
    const counts = { implemented: 0, awaitingA5: 0, certified: 0, pending: 0 };
    for (const l of content.split('\n')) {
        if (/^\s*[-*] \[x\]\s/i.test(l)) {
            counts.implemented++;
            if (AWAIT_A5_RE.test(l)) counts.awaitingA5++;
            if (CERTIFIED_RE.test(l)) counts.certified++;
        } else if (/^\s*[-*] \[ \]\s/i.test(l)) {
            counts.pending++;
        }
    }
    return counts;
}

console.log('# Engine Status Report\n');
console.log(`Generated: ${new Date().toISOString()}\n`);
console.log('| Agent | Implementadas | Aguardando A5 | Certificadas | Pendentes | Total | % |');
console.log('|-------|---------------|---------------|--------------|-----------|-------|---|');

let tImpl = 0, tAwait = 0, tCert = 0, tPending = 0;

for (const dir of agentDirs) {
    const tp = join(dir, 'task_plan.md');
    try {
        const content = readFileSync(tp, 'utf8');
        const c = countItems(content);
        const total = c.implemented + c.pending;
        const pct = total > 0 ? Math.round(c.implemented / total * 100) : 0;
        const name = dir.split('/').pop().replace('agente', 'Agent ').replace(/_/g, ' ');
        console.log(`| ${name} | ${c.implemented} | ${c.awaitingA5} | ${c.certified} | ${c.pending} | ${total} | ${pct}% |`);
        tImpl += c.implemented; tAwait += c.awaitingA5; tCert += c.certified; tPending += c.pending;
    } catch (e) {
        console.log(`| ${dir} | ERROR | - | - | - | - | - |`);
    }
}

const totalAll = tImpl + tPending;
const totalPct = totalAll > 0 ? Math.round(tImpl / totalAll * 100) : 0;
console.log(`| **TOTAL** | **${tImpl}** | **${tAwait}** | **${tCert}** | **${tPending}** | **${totalAll}** | **${totalPct}%** |`);

// Bugs summary — any table row whose status is different from exactly RESOLVIDO
// (plan H: não filtrar por prefixo `BUG-*`, não ignorar estados fora de RESOLVIDO).
console.log('\n## Bugs não resolvidos\n');
console.log('| Agent | ID | Severity | Status | Description |');
console.log('|-------|-----|----------|--------|-------------|');

let openBugs = 0;
for (const dir of agentDirs) {
    const bugsFile = join(dir, 'bugs.md');
    try {
        const content = readFileSync(bugsFile, 'utf8');
        const lines = content.split('\n').filter(l => /^\s*\|/.test(l));
        for (const line of lines) {
            // Canonical row layout (agentes/**/bugs.md):
            //   | BUG-0NN | <STATUS> | <SEVERITY> | <item> | <causa> ... |
            const parts = line.split('|').map(s => s.trim()).filter(Boolean);
            if (parts.length < 3) continue;
            const status = (parts[1] || '').toUpperCase();
            const headerLike = new Set(['ID', 'STATUS', '---']);
            if (headerLike.has(parts[0]) || headerLike.has(status)) continue;
            if (status === 'RESOLVIDO') continue;   // only fully-resolved rows are excluded
            openBugs++;
            const agent = dir.split('/').pop().replace('agente', 'A').replace(/_.*/, '');
            const desc = parts.slice(3).join(' · ').replace(/\|/g, ' ').substring(0, 60);
            console.log(`| ${agent} | ${parts[0]} | ${parts[2]} | ${parts[1]} | ${desc} |`);
        }
    } catch (e) { /* skip */ }
}
console.log(`\nBugs em aberto (qualquer status != RESOLVIDO): **${openBugs}**`);
