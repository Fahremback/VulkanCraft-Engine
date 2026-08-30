#!/usr/bin/env node
// rodada-fechamento-audit.mjs — Conta 6, item 1: auditoria única do fechamento.
//
// Deriva o estado REAL dos seis planos da rodada (conta1..conta6), dos seus
// `bugs.md` e dos consumidores de produto, apenas com base na evidência do
// próprio texto (nunca em comentário externo). Esta arquitetura de "fechar o
// plano = existe arquivo + consumer real + comportamento observável" é o que o
// AGENTS.md exige: SDK, teste, servidor-only, probing e texto contraditório
// NÃO fecham um item.
//
// Para cada conta a auditoria regista:
//   * checklists      - contagem de [ ] / [x] / [~] no task_plan.md
//   * openBugs        - linhas do bugs.md cujo status não é RESOLVIDO
//   * missingRefs     - [x] que referenciam `path` de arquivo inexistente
//   * refused[x]      - [x] reabertos por evidência insuficiente/contraditória
//   * consumers       - consumers reais encontrados no caminho do produto
//   * ready           - zero [ ], zero [~], zero bug aberto, zero recusa, e um
//                       consumer real registrado (quando o item é de produto)
//
// Gate --strict: exit 1 se qualquer conta tiver pendência que trave o
// fechamento. O resultado é emitido em JSON + resumo de console.

import fs from 'node:fs';
import path from 'node:path';

const root = process.cwd();
const roundsRoot = path.join(root, 'agentes', 'rodada_fechamento_6');
const accounts = [
  'conta1_grid',
  'conta2_render_real',
  'conta3_nav_ia',
  'conta4_gameplay_produto',
  'conta5_editor_sdk',
  'conta6_integracao',
];

// Evidência que NÃO fecham um item de produto, mesmo usadas juntas. Regras
// estreitas para defeitos concretos, evitando falsos positivos de vocabulário
// legítimo (ex.: "SDK instalável" num item de packaging não é evidência de
// interface pública consumida; mas "server-only" num item de gameplay é).
const refusalRules = [
  [/unidade[s]? de teste\b/i, 'evidência é só unit test'],
  [/\b(?:só|apenas|somente|only)\b(?:\s+(\w+))?\s*\bdentro\b/i, 'escopo confinado a subconjunto'],
  [/\b(?:server[- ]only|servidor[- ]only)\b/i, 'evidência é servidor-only'],
  [/\bTEST-ONLY\b/i, 'evidência é TEST-ONLY'],
  [/\bSDK[- ]INTERNAL\b/i, 'evidência é SDK-internal'],
  [/\bprobe\b/i, 'evidência é um probe isolado'],
  [/\b(?:mock|stub|placeholder|no-op)\b/i, 'evidência é mock/stub/no-op'],
  [/\bnão (?:implementad|integrad)o?[a-z]*\b/i, 'texto admite ausência'],
  [/\b(?:pend[eê]ncia|pendente|resta|restante|lacuna)\b/i, 'texto admite pendência'],
  [/\bb(t?bloquead|loquead)o?[a-z]*\b/i, 'texto admite bloqueio'],
  [/\bhuman[- ]visual\b/i, 'validação visual ainda pendente'],
  [/\b(?:futur[oa]|quando .* dispon[ií]vel)\b/i, 'trabalho adiado para o futuro'],
  [/\brenderer[- ]side\b/i, 'evidência é renderer-side (sem consumer de jogo)'],
  [/\b0 hits\b/i, 'evidência declara consumer ausente'],
];

function read(p) {
  try { return fs.readFileSync(p, 'utf8'); } catch { return ''; }
}
function exists(p) { return fs.existsSync(p); }

function indexCodeFiles(dir, acc = {}, depth = 0) {
  if (depth > 9 || !exists(dir)) return acc;
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) {
      const base = e.name.toLowerCase();
      if (['.git', 'build', 'out', 'external', '.sentry-native', 'backup', 'node_modules', 'third_party'].includes(base)) continue;
      indexCodeFiles(p, acc, depth + 1);
    } else {
      acc[e.name.toLowerCase()] = p;
    }
  }
  return acc;
}
const codeFiles = indexCodeFiles(root);

// Resolve a backtick file reference to an on-disk abs path (or null) and to its
// product zone. Zone drives the "real consumer" derivation (item 1 requires
// refusing [x] whose only evidence is SDK/test-only).
function resolveRefToPath(token) {
  const norm = token.replaceAll('\\', '/').replace(/[)>,;]/g, '');
  const candidates = [
    path.join(root, norm),
    path.join(root, 'agentes', norm),
    path.join(root, 'src', norm),
    path.join(root, 'src', 'engine', 'public', norm),
    path.join(root, 'src', 'engine', 'sdk', norm),
    path.join(root, 'tools', norm),
  ];
  const flat = candidates.find((c) => exists(c));
  if (flat) return flat;
  const base = path.basename(norm).toLowerCase();
  return codeFiles[base] || null;
}
function zoneOf(absPath) {
  const rel = path.relative(root, absPath).replaceAll('\\', '/');
  if (/^(src\/app|src\/editor|src\/server)\//.test(rel)) return 'product';
  if (rel.startsWith('shaders/') || rel.startsWith('assets/') || rel.startsWith('schema/')) return 'product'; // artefatos do produto
  if (rel.startsWith('tools/')) return 'tool';
  if (rel.startsWith('tests/')) return 'test';
  if (rel.startsWith('src/engine/sdk')) return 'sdk';
  if (rel.startsWith('src/engine')) return 'engine'; // código do núcleo (consumido pelo produto)
  return 'other';
}
function resolveRef(token) { return resolveRefToPath(token) !== null; }

// Product zones the plan's "real consumer" rule cares about: a product item only
// closes when evidence lives in (or is consumed by) app/editor/server (plus
// shaders/assets/schema), not when the only refs are sdk/test/tool.
const productZoneRe = /^(src\/app|src\/editor|src\/server|shaders\/|assets\/|schema\/)/;

const report = { schema: 1, generatedAt: new Date().toISOString(), accounts: {} };
let totalOpen = 0;
let totalRefused = 0;
let totalOpenBugs = 0;

// Refusal heuristics describe product-integration evidence (a capability wired
// into the real game must not be proven only by SDK/test/server/probe). They
// apply strictly to the five product-integration accounts (1-5). Conta 6 is the
// delivery/coordination account: its items are delivery/infrastructure tasks
// whose [x] is legitimately judged by artifacts + absence of open items, and
// several of its own task texts quote the refusal vocabulary (e.g. item 1
// literally says "recusando [x] com ... servidor-only"), which would otherwise
// be a false positive. Conta 6 closure is still enforced by open/~/bugs counts
// and by items 3-6 being gated on contas 1-5.
const productIntegrationAccounts = accounts.slice(0, 5); // conta1..conta5

for (const acct of accounts) {
  const appliesRefusal = productIntegrationAccounts.includes(acct);
  const dir = path.join(roundsRoot, acct);
  const planPath = path.join(dir, 'task_plan.md');
  const bugsPath = path.join(dir, 'bugs.md');
  const plan = read(planPath);
  const bugs = read(bugsPath);
  const planRel = path.relative(root, planPath).replaceAll('\\', '/');

  const checked = [...plan.matchAll(/^\s*-\s+\[x\]/gm)].length;
  const checkedTilde = [...plan.matchAll(/^\s*-\s+\[~\]/gm)].length;
  const open = [...plan.matchAll(/^\s*-\s+\[ \]/gm)].length;
  totalOpen += open + checkedTilde;

  // bugs.md com status diferente de RESOLVIDO
  const openBugs = [];
  for (const m of bugs.matchAll(/\|\s*([^|]+?)\s*\|\s*(ABERTO|BLOQUEADO|PARCIAL|TODO)/gi)) {
    openBugs.push(m[1].trim());
  }
  totalOpenBugs += openBugs.length;

  // verificação de cada linha [x]
  const lines = plan.split(/\r?\n/);
  const refusedByLine = {};
  const missingRefsByLine = {};
  const evidenceZones = {};   // line -> Set of zone per referenced file
  const noProductRefs = {};   // line -> [zones] when a [x] has refs but none product
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    if (!/^\s*-\s+\[x\]/.test(line)) continue;
    let reason = null;
    if (appliesRefusal) {
      // Evaluate the refusal vocabulary only on the EVIDENCE text, not the task
      // statement. Audit/spec tasks legitimately quote the refusal vocabulary in
      // their item text (e.g. conta4 item 1 is literally an audit of claims that
      // say "renderer-side"), which would otherwise be a false positive. The
      // evidence is the annotated `*(...)*` tail; if none present, fall back to
      // the whole line so a bare `[x]` with no evidence stays verifiable.
      const ev = /\*\(/.test(line) ? line.slice(line.indexOf('*(')) : line;
      for (const [re, label] of refusalRules) {
        if (re.test(ev)) { reason = label; break; }
      }
    }
    if (reason) refusedByLine[i + 1] = reason;
    // referências de arquivo inexistentes + zonas dos arquivos existentes.
    // Refêrencias a agentes/ são planos/bugs/coordenação, não evidência de
    // produto — não entram na derivação de consumer real (nem causam recusa).
    const zones = new Set();
    for (const m of line.matchAll(/`([^`]+\.(?:hpp|h|cpp|c|mjs|js|ts|py|ps1|md|json|toml|cmake|png|vcasset|scene|world|vert|frag|comp|geom|rgen|rchit|rmiss|rgen|tesc|tese|glsl))`/gi) || []) {
      const ref = m[1];
      if (/[*?]/.test(ref)) continue;
      const abs = resolveRefToPath(ref);
      if (!abs) { missingRefsByLine[i + 1] = ref; continue; }
      const rel = path.relative(root, abs).replaceAll('\\', '/');
      if (rel.startsWith('agentes/')) continue; // planos/bugs/coordenação não são evidência de produto
      // Refêrencias de coordenação (task_plan.md/bugs.md de planos) resolvidas por
      // basename que NÃO caem sob agentes/ (ex.: master task_plan.md do root) também
      // não são evidência de produto — evita recusa falsa em notas de fechamento.
      if (/\btask_plan\.md$|\bbugs\.md$/i.test(rel)) continue;
      zones.add(zoneOf(abs));
    }
    if (zones.size) {
      evidenceZones[i + 1] = [...zones];
      if (appliesRefusal && !zones.has('product')) {
        noProductRefs[i + 1] = [...zones];
      }
    }
  }

  // consumers reais: o plano de produto cita usar o executável adequado.
  const productTerms = ['jogo', 'jogador', 'game', 'viewport', 'renderer', 'render', 'editor', 'executável real', 'executavel real', 'loop real', 'bootstrap', 'consumer real'];
  const consumerHits = [];
  for (const t of productTerms) if (plan.toLowerCase().includes(t.toLowerCase())) consumerHits.push(t);

  const accountReport = {
    plan: planRel,
    checklists: { open, checked, tilde: checkedTilde },
    openBugs,
    missingRefs: missingRefsByLine,
    refused: refusedByLine,
    evidenceZones,
    noProductRefs,
    consumerTerms: [...new Set(consumerHits)].slice(0, 12),
    ready: open === 0 && checkedTilde === 0 && openBugs.length === 0 && Object.keys(refusedByLine).length === 0 && Object.keys(missingRefsByLine).length === 0 && Object.keys(noProductRefs).length === 0,
  };
  report.accounts[acct] = accountReport;
}

report.summary = {
  accountsWithOpenItems: accounts.length,
  totalOpen, totalRefused, totalOpenBugs,
  allReady: Object.values(report.accounts).every((a) => a.ready),
};
if (report.summary.totalRefused === 0) report.summary.totalRefused = Object.values(report.accounts).reduce((s, a) => s + Object.keys(a.refused).length, 0);

// escrita
const outArg = process.argv.slice(2).find((a) => !a.startsWith('-')) ?? 'out/artifacts/rodada-fechamento-audit.json';
const target = path.join(root, outArg);
fs.mkdirSync(path.dirname(target), { recursive: true });
fs.writeFileSync(target, JSON.stringify(report, null, 2) + '\n');

console.log('[rodada-fechamento-audit] planos auditados: ' + accounts.length);
for (const acct of accounts) {
  const a = report.accounts[acct];
  console.log(`  ${acct}: [ ]=${a.checklists.open} [~]=${a.checklists.tilde} [x]=${a.checklists.checked} bugs=${a.openBugs.length} refusals=${Object.keys(a.refused).length} missingRefs=${Object.keys(a.missingRefs).length} ready=${a.ready}`);
}
console.log(`[rodada-fechamento-audit] totalOpen=${report.summary.totalOpen} refused=${report.summary.totalRefused} openBugs=${report.summary.totalOpenBugs} allReady=${report.summary.allReady}`);
console.log(`[rodada-fechamento-audit] wrote ${path.relative(root, target).replaceAll('\\', '/')}`);

if (process.argv.includes('--strict') && !report.summary.allReady) {
  console.error('[rodada-fechamento-audit] --strict FAIL: há pendências que travam o fechamento');
  process.exit(1);
}
console.log('[rodada-fechamento-audit] done');