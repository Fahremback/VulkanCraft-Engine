// Reopens checklist claims whose own evidence proves that the requested work
// is incomplete. Keep rules keyed by the immutable task text, never line number.
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(import.meta.dirname, '..', '..');
const selectedAgentsArg = process.argv.find(arg => arg.startsWith('--agents='));
const selectedAgents = selectedAgentsArg
  ? new Set(selectedAgentsArg.slice('--agents='.length).split(',').filter(Boolean))
  : null;
const rules = {
  agente1_render_lumen: [
    'Integrar RTXDI/ReSTIR DI:', 'Integrar RTXGI/DDGI/radiance probes',
    'Integrar NRD para GI', 'Integrar FidelityFX aplicável',
    'Implementar reflexos por roughness', 'Integrar sombras, materiais voxel',
    'Integrar atmosfera, céu', 'Criar debug views:',
    'Remover caminhos builtin-only do renderer', 'Fazer renderer visualizar fluidos',
    'Integrar efeitos de abilities', 'Garantir resize, fullscreen',
    'Garantir HDR, tonemapping', 'Integrar instancing/meshlets',
    '`atmospheric-scattering`:', '`effekseer`:', '`embree`:', '`fidelityfx-sdk`:', '`ktx-software`:',
    '`nrd`:', '`openxr-sdk-source`:', '`rtxdi`:', '`rtxgi`:', '`slang`:',
    '`tressfx`:', '`vkfft`:', '`volk`:', '`vulkan-samples`:',
    'Cena interna sem luz fica escura', 'Handoff documentado para Agentes 2, 5 e 6'
  ],
  agente2_editor_ui: [
    'Implementar escala, safe area, DPI', 'Expor composição UI no editor',
    'Escolher e fechar uma única camada UI', 'Implementar shell viewport-first',
    'Integrar Hierarchy, Content Browser, Inspector',
    'Copiar/adaptar o frontend útil', 'Implementar `ContentBrowserWindow`',
    'Integrar profiler/render debugger', 'Cobrir DPI, múltiplos aspectos',
    'Fazer mutações passarem pelo command bus',
    'Adicionar testes text-only de layout, interação e framebuffer',
    'Criar painel visual de retargeting', 'Refatorar editor em `EditorShell`',
    'Implementar onboarding, tutorial interno', 'Implementar import→cook→inspect→edit→play→package→publish',
    'Integrar inspector/authoring', 'Editor abre projeto, importa assets',
    'Produzir screenshot hero', '`ezengine`:', '`rmlui`:', '`freetype`:', '`harfbuzz`:',
    '`fastgltf`:', '`libjpeg-turbo`:', '`libspng`:', '`openimageio`:',
    '`openusd`:', '`materialx`:', '`glaze`:',
    'UI funciona em resoluções/aspectos/DPI/controle diferentes',
    'Testes text-only e GPU do editor passam'
  ],
  agente3_voxel_world: [
    'Handoffs para Agentes 1, 4, 5 e 6 estão verdes'
  ],
  agente4_gameplay_ai: [
    'Unificar player, mobs, veículos', 'Implementar ciclo completo spawn, despawn',
    'Completar atualização localizada da navegação', 'Implementar off-mesh links',
    'Implementar pathfinding hierárquico', 'Implementar consultas assíncronas',
    'Suportar agentes com diferentes tamanhos', 'Integrar navegação ao streaming',
    'Expor debug draw, métricas e consultas públicas',
    'Integrar entidades à partição espacial', 'Integrar IA a navegação',
    'Expor authoring, visualização, profiling', 'Integrar LOD de animação',
    'Integrar physical animation, active ragdoll', 'Implementar LOD de animação',
    'Completar character controller estável', 'Implementar ragdoll configurável',
    'Integrar dano/deformação/fragmentação', 'Integrar poderes/abilities ao cenário',
    'Completar Ability System data-driven', 'Integrar WorldManager do Agente 3',
    'Garantir que snapshots de tempo', 'Integrar materiais, impactos, passos',
    'Expor authoring, profiler e testes determinísticos',
    'Publicar assets, componentes, reflection', 'Expor replay determinístico',
    'Publicar hooks visuais/sonoros', '`behavior-tree-cpp`:', '`ceres-solver`:',
    '`opus`:', '`steam-audio`:',
    'Cada sistema possui contrato público, asset/schema',
    'Handoffs para Agentes 2, 3, 5 e 6 estão verdes'
  ],
  agente5_sdk_mcp: [
    'Consolidar networking público:',
    'Implementar operações longas com job ID', '`agones`:', '`rocksdb`:',
    'Cada capacidade pública é acessível coerentemente',
    'Handoffs com Agentes 1–4 e 6 estão verdes'
  ],
  agente6_integracao: [
    'Testar Render Graph, shaders, materiais, resize',
    'Executar validation layers e testes GPU',
    'Testar UI em resoluções, aspectos, escalas',
    'Preparar screenshots/GIFs e exemplos reproduzíveis',
    'Mover scripts soltos para o subdiretório correto',
    'Consumidor avançado usa voxel, block entities',
    'Criar replay/cenas douradas determinísticas',
    '`fuzztest`:', '`tracy`:', '`vcpkg`:',
    'Remover marcações falsas de conclusão por mera presença',
    'Consolidar schemas versionados para projetos',
    'Consolidar scripting com sandbox',
    'Completar networking público para servidor dedicado',
    'Validar instalação e uso do SDK/MCP em caminho com espaços',
    'Resolver todos os bugs e handoffs desta missão',
    'Executar soak, stress, fuzz, sanitizers', 'Criar matriz CI Windows/Linux',
    'Adicionar análise estática, formatação verificável', 'Integrar fuzzing aos parsers',
    'Integrar sanitizers/validation', 'Verificar nenhum segredo, path absoluto',
    'Organizar `docs` com índice único', '`concurrentqueue`:', '`mimalloc`:',
    '`spdlog`:', '`taskflow`:', 'Todos os task plans dos Agentes 1–6 estão 100%',
    'Build limpo, testes, validation', 'Performance, memória, streaming',
    'Entrega final contém provas', 'Revalidar todos os `[x]` anteriores',
    'Auditar os `bugs.md` dos seis agentes', 'Auditar cada checklist dos Agentes 1–5',
    'Integrar de verdade `concurrentqueue`', 'Reduzir o resultado de `tools/maintenance/layout-audit.ps1',
    'Consolidar builds fora da árvore', 'Executar a matriz real de presets',
    'Executar validação real no Windows',
    'Executar de fato ASan no Windows', 'Executar os testes de janela/GPU/editor',
    'Verificar frescor dos executáveis', 'Validar o repositório público em clone limpo',
    'Substituir o Gate Final anterior', 'Executar benchmarks e stress tests com milhares',
    'Integrar ferramentas de depuração no editor', 'Validar save/load, hot reload, replication',
    'Resolver e fechar todos os bugs criados', 'Entregar ao Agente 4 os contratos',
    'Completar o MCP para criar, abrir', 'Expor pelo MCP gameplay, IA',
    'Validar que uma IA consegue criar cada template', 'Unificar asset pipeline entre editor',
    'Adicionar testes de contrato garantindo paridade', 'Executar fuzzing dos parsers, schemas',
    'Manter build, CTest, package', 'Gerar menu, configurações, input remapeável',
    'Gerar mundo determinístico com biomas', 'Permitir edição voxel transacional incluindo',
    'Integrar personagem, colisão voxel', 'Criar criatura com percepção',
    'Criar veículo montável', 'Criar ability que altera cenário',
    'Criar explosão que afeta voxels', 'Gerar cliente, servidor dedicado',
    'Rejeitar posição impossível', 'Encerrar processos durante escrita',
    'Migrar três versões artificiais', 'Alterar material, shader, UI, áudio',
    'Instalar, atualizar, desativar e remover plugin', 'Injetar falha/timeout em plugin',
    'Cobrir no mínimo 95% das APIs públicas', 'Gerar relatório HTML único com capturas',
    'Publicar o projeto certificado como exemplo oficial', 'Auditar as provas:'
  ]
};

const changed = [];
for (const [agent, needles] of Object.entries(rules)) {
  if (selectedAgents && !selectedAgents.has(agent)) continue;
  const file = path.join(root, 'agentes', agent, 'task_plan.md');
  let text = fs.readFileSync(file, 'utf8');
  const lines = text.split(/\r?\n/);
  for (let i = 0; i < lines.length; ++i) {
    if (!/^\s*- \[[xX]\]/.test(lines[i])) continue;
    const needle = needles.find(n => lines[i].includes(n));
    if (!needle) continue;
    let reason = 'a entrega integral ainda não possui prova atual';
    if (/HUMAN[- ]VISUAL|visual.*pendente|renderização visual.*pendente/iu.test(lines[i])) {
      reason = 'o próprio registro deixa integração/validação visual pendente; núcleo headless não conclui o requisito';
    } else if (/não vendad|lib .*não vendad|vendor .*pending|vendor .*pendente/iu.test(lines[i])) {
      reason = 'o repositório solicitado não foi integrado; implementação substituta não conclui esta tarefa de integração';
    } else if (/DONE[- ](?:núcleo|parcial)|NÚCLEO ENTREGUE|core-only/iu.test(lines[i])) {
      reason = 'somente o núcleo/subconjunto foi entregue';
    } else if (/handoff|sem bug aberto/iu.test(lines[i])) {
      reason = 'há dependências ou bugs cruzados ainda abertos';
    }
    lines[i] = lines[i].replace('[x]', '[ ]').replace('[X]', '[ ]');
    if (!lines[i].includes('AUDITORIA 2026-08-27 — REABERTO')) {
      lines[i] += ` *(AUDITORIA 2026-08-27 — REABERTO: ${reason}.)*`;
    }
    changed.push({ agent, line: i + 1, task: needle });
  }
  fs.writeFileSync(file, lines.join('\n'), 'utf8');
}

const report = {
  generatedAt: new Date().toISOString(),
  reopenedThisRun: changed.length,
  auditedReopened: Object.fromEntries(Object.entries(rules).map(([agent, needles]) => {
    const file = path.join(root, 'agentes', agent, 'task_plan.md');
    const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/);
    return [agent, needles.filter(needle => lines.some(line => /^\s*- \[ \]/.test(line) && line.includes(needle))).length];
  })),
  byAgent: Object.fromEntries(Object.keys(rules).map(agent => [
    agent, changed.filter(item => item.agent === agent).length
  ])),
  reasons: [
    'evidence explicitly admits pending, partial, blocked, core-only or handoff work',
    'a declared integration is only a substitute contract/probe, not wired to the product',
    'Linux/GPU/display/runtime execution was claimed from a script or headless model only',
    'the global gate was checked while open tasks and bugs still exist'
  ],
  changed
};
report.auditedReopenedTotal = Object.values(report.auditedReopened).reduce((sum, value) => sum + value, 0);
const reportPath = path.join(root, 'agentes', 'AUDITORIA_CONCLUSOES_ATUAL.json');
fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n', 'utf8');
console.log(JSON.stringify(report, null, 2));
