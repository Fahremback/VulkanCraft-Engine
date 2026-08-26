#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const engineRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const agentsRoot = path.join(engineRoot, "agentes");
const apply = process.argv.includes("--apply");
const plans = fs.readdirSync(agentsRoot, { withFileTypes: true })
  .filter((entry) => entry.isDirectory() && /^agente[1-6]_/.test(entry.name))
  .map((entry) => path.join(agentsRoot, entry.name, "task_plan.md"))
  .sort();

const incompleteRules = [
  [/DONE[- ](?:n[uú]cleo|parcial)/iu, "marcada como núcleo/parcial"],
  [/\bCORE(?:\s*\[x\])?/iu, "somente core entregue"],
  [/HUMAN[- ]VISUAL/iu, "validação visual ainda pendente"],
  [/\b(?:pend[eê]ncia|pendente|resta|restante|lacuna)s?\b/iu, "o próprio texto admite pendência"],
  [/\bbloquead[oa]s?\b/iu, "o próprio texto admite bloqueio"],
  [/n[aã]o (?:implementad|integrad|exist)[oa]s?/iu, "implementação/integração ausente"],
  [/(?:ready for integration|available in|not installed|requires display|requer display|save pendente)/iu, "disponibilidade foi tratada como integração"],
  [/\b(?:futura|futuro|future optimization|quando .* dispon[ií]vel)\b/iu, "trabalho adiado para o futuro"],
  [/sem bug aberto (?:no|em) (?:meu )?dom[ií]nio/iu, "filtrou bugs por interpretação de domínio"],
  [/\b0 (?:hits|ferramentas|refs|refer[eê]ncias)\b/iu, "evidência declara consumidor ausente"],
  [/\b(?:mock|stub|placeholder|no-op)\b/iu, "entrega baseada em mock/stub/no-op"],
];

const evidencePattern = /(?:PASSED|ALL PASSED|exit\s*=?\s*0|\bteste?s?\b|\bgate\b|\bsmoke\b|\bchecksum\b|\bcommit\b|\bverificad[oa]\b|\bevid[eê]ncia\b|`[^`]+\.(?:hpp|h|cpp|c|mjs|js|ts|py|ps1|md|json|toml|cmake)`|\bCMakeLists\b|\b\d+\/\d+\b)/iu;

const reopened = [];
let checkedBefore = 0;

for (const plan of plans) {
  const original = fs.readFileSync(plan, "utf8");
  const lines = original.split(/\r?\n/);
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    if (!/^\s*- \[x\]/iu.test(line)) continue;
    checkedBefore += 1;
    let reason = null;
    for (const [pattern, label] of incompleteRules) {
      if (pattern.test(line)) {
        reason = label;
        break;
      }
    }
    if (!reason && !evidencePattern.test(line)) {
      reason = "sem evidência verificável registrada na própria conclusão";
    }
    if (!reason) continue;
    reopened.push({
      file: path.relative(engineRoot, plan).replaceAll("\\", "/"),
      line: index + 1,
      reason,
      text: line.trim(),
    });
    lines[index] = line.replace(/^(\s*- )\[x\]/iu, "$1[ ]");
  }
  if (apply) fs.writeFileSync(plan, `${lines.join("\n")}\n`, "utf8");
}

const byAgent = new Map();
for (const item of reopened) {
  const agent = item.file.split("/")[1];
  byAgent.set(agent, (byAgent.get(agent) ?? 0) + 1);
}

if (apply) {
  const report = [
    "# Auditoria de conclusões dos agentes",
    "",
    "Uma tarefa foi reaberta quando o próprio `[x]` admitia trabalho faltante/bloqueado/parcial ou quando não registrava evidência verificável. Reabrir não afirma que todo código está ausente; afirma que a conclusão integral não foi provada.",
    "",
    `- Verificadas: ${checkedBefore} marcações \`[x]\``,
    `- Reabertas: ${reopened.length}`,
    `- Preservadas com evidência sem contradição: ${checkedBefore - reopened.length}`,
    "",
    "## Reaberturas",
    "",
    ...reopened.map((item) => `- \`${item.file}:${item.line}\` — ${item.reason}.`),
    "",
  ];
  fs.writeFileSync(path.join(agentsRoot, "AUDITORIA_CONCLUSOES.md"), report.join("\n"), "utf8");
}

console.log(JSON.stringify({ apply, checkedBefore, reopened: reopened.length, preserved: checkedBefore - reopened.length, byAgent: Object.fromEntries(byAgent) }, null, 2));
