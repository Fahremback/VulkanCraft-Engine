# VulkanCraft Engine MCP

Servidor MCP local e portátil, sem dependências externas, para agentes de IA criarem jogos usando os recursos públicos da VulkanCraft Engine. A manutenção da própria engine também está disponível, com proteção contra edições concorrentes.

## Executar

```powershell
node <engine>/tools/mcp-server/server.mjs
```

Transporte remoto opcional (HTTP + SSE), para clientes que não usam stdio ou que precisam de múltiplos clientes/reconexão:

```powershell
node <engine>/tools/mcp-server/server.mjs --http [--port 8322]
# POST /mcp   -> JSON-RPC (resposta na mesma conexão)
# GET  /events -> SSE (notifications/* transmitidas a todo cliente conectado)
```

O servidor é `stdio`-first (o transporte padrão do MCP `command`/`args`); o modo HTTP é aditivo e opcional, escuta em `127.0.0.1`, e para de forma limpa com `SIGINT`/`SIGTERM`. Subscriptions de eventos sobrevivem à reconexão do SSE (basta reabrir `/events`).

## Configuração MCP

```json
{
  "mcpServers": {
    "vulkancraft-engine": {
      "command": "node",
      "args": [
        "<engine>\\tools\\mcp-server\\server.mjs"
      ]
    }
  }
}
```

**Auth opcional no transporte remoto** (`--http`): defina `MCP_AUTH_TOKEN` no ambiente do servidor para exigir `Authorization: Bearer <token>` em todo `POST /mcp` e `GET /events` (HTTP 401 sem token válido). Off por default — stdio (pipe local) nunca exige auth (findings #240).

## CLI de registry assets (FALTANTES item 10)

Sem servidor MCP, o mesmo contrato está disponível em linha de comando (`registry-cli.mjs`, sem dependências — reusa exatamente as factories do servidor):

```
node <engine>/tools/mcp-server/registry-cli.mjs kinds
node <engine>/tools/mcp-server/registry-cli.mjs schema <kind>            # JSON Schema draft-07
node <engine>/tools/mcp-server/registry-cli.mjs export-schemas [outDir] # escreve schema/registry/<kind>.json
node <engine>/tools/mcp-server/registry-cli.mjs validate <kind> <file.json>   # exit 1 em documento inválido
node <engine>/tools/mcp-server/registry-cli.mjs author --engine <root> --project <nome> --kind <kind> --name <n> --file <doc.json> [--dry-run] [--update]
```

Os schemas exportados (draft-07) são o artefato para editor/IDE (intellisense), scripting e CI validarem os documentos de `Content/Registry/`; o `game_capabilities` do MCP também os serve como `registry_schemas`. Gerados do mesmo `REGISTRY_FIELD_SCHEMAS` que a autoria espelha — uma única fonte de verdade.

## Ferramentas

### Controle do editor ao vivo (Control API, 69 ferramentas)

Todas as funcionalidades interativas do editor rodando (o mesmo conjunto da Control API HTTP em `127.0.0.1:8321`) estão expostas como ferramentas MCP tipadas — o agente pode **dirigir o editor aberto** em vez de só escrever arquivos:

- **Estado**: `editor_status` (JSON ao vivo de `/state` + `/health`).
- **Play/Simulação**: `editor_play`, `editor_pause`, `editor_resume`, `editor_step` (PASSO de 1 frame), `editor_stop`, `editor_simulate`.
- **Câmera**: `editor_camera_zoom`, `editor_camera_move`, `editor_camera_turn`, `editor_camera_focus`.
- **Cena**: `editor_new_scene`, `editor_open_scene`, `editor_save_scene`, `editor_add_entity` (16 tipos), `editor_select_entity`, `editor_delete_entity`, `editor_rename_entity`, `editor_set_transform`, `editor_add_component` (15 tipos).
- **Gizmos**: `editor_set_gizmo`, `editor_set_gizmo_space`, `editor_set_snap`.
- **Assets**: `editor_import_asset`, `editor_create_block_model`, `editor_spawn_block`, `editor_duplicate_asset`, `editor_delete_asset`, `editor_reimport_asset`.
- **Voxel**: `editor_voxel_generate`, `editor_voxel_paint`, `editor_voxel_clear`.
- **Scripts**: `editor_script_event`, `editor_script_pause`, `editor_script_continue`, `editor_script_step`.
- **Janelas/Editores/Tema/Clima**: `editor_toggle_window` (42 janelas), `editor_open_specialized_editor` (16 abas), `editor_set_theme`, `editor_set_weather`.
- **Terreno/Gráficas/Projeto/Malha/Dev**: `editor_generate_terrain`, `editor_set_graphics`, `editor_save_settings`, `editor_create_project`, `editor_apply_mesh`, `editor_run_self_test` (5 testes, timeout 120s), `editor_package_assets`, `editor_hot_reload`.

Cada resultado devolve `endpoint` e `command` para o agente repetir via curl e manter trilha. Se o editor não estiver aberto, as ferramentas respondem com erro claro: *"start VulkanEngineEditor first"* — nunca travam.

### Criação de jogos sem alterar a engine

- `game_capabilities`: contrato de recursos, componentes e scripts suportados.
- `list_game_projects`: projetos disponíveis.
- `create_game_project`: projeto portátil com configuração e cena inicial.
- `inspect_game_project`: conteúdo e validação do projeto.
- `create_scene` / `inspect_scene`: cenas no formato nativo da engine.
- `create_entity` / `remove_entity`: entidades da cena.
- `set_component` / `remove_component`: componentes públicos da engine.
- `create_visual_script`: scripts visuais tipados no formato nativo.
- `create_material`: materiais PBR nativos.
- `create_audio_event`: eventos de áudio com variação e espacialização.
- `create_physics_material`: materiais físicos nativos.
- `stage_asset`: assets e import settings portáteis.
- `author_registry_asset`: assets de registry (block/item/fluid/recipe/biome/structure) como JSON versionado em `Content/Registry/<kind>/<name>.json`, espelhando exatamente os schemas JSON que as factories públicas C++ parseiam. Aceita `dry_run: true` (valida e pré-visualiza o documento/diff sem gravar) e `update: true` (substitui devolvendo o documento anterior para rollback). Assets inválidos são recusados com diagnósticos estruturados.
- `inspect_registry_assets`: lista e valida todos os assets de registry de um projeto.
- `validate_game_project`: validação sem compilar ou modificar a engine (inclui assets de registry).

### Assets de registry (FALTANTES §23)

Cada documento é validado estruturalmente contra as regras das factories públicas (`BlockRegistry`/`ItemRegistry`/`FluidRegistry`/`RecipeRegistry`, `IBiomeRegistry`, `IStructureGenerator`). O gate de equivalência `mcp_registry_gate_tests` prova que os documentos emitidos carregam pelas factories públicas sem alteração; `protocol-smoke.mjs` cobre os seis kinds, dry-run e recusas.

Observações honestas do contrato atual:
- Blocos são **catalog-only**: declarar `builtin_id` é recusado porque a tabela builtin ocupa todos os ids 0..50 (`BlockType::Count`), então qualquer mapeamento falharia no C++ como "already used"/"out of range".
- Receitas validam estrutura no MCP; as referências de item/tag são validadas pelo `RecipeRegistry` C++ contra um `ItemRegistry` registrado.
- Valores que o C++ clamparia silenciosamente (viscosity 0..1, range 1..7, damagePerTick ≥ 0, time/energy ≥ 0, chance em (0,1]) são recusados no MCP para o asset autorado round-trip bit-exato.

### Manutenção da engine

- `engine_overview`: mapa compacto da arquitetura e targets.
- `engine_pending_work`: pendências reais consolidadas.
- `list_directory`: navegação limitada por profundidade e quantidade.
- `search_code`: busca literal ou regex limitada.
- `inspect_symbol`: declarações e usos de símbolos C/C++.
- `read_file`: leitura parcial com SHA-256.
- `apply_text_edits`: substituições exatas, atômicas e protegidas contra concorrência.
- `create_file`: criação atômica de arquivos textuais.

### Validação sem janela (run + logs)

- `build_game`: compila um target Release via cmake e grava a saída completa em `Projects/.runs/build-<target>-<ts>.log`; devolve status, linhas de erro e tail.
- `run_game`: executa um exe já compilado (VulkanEngineGame/Editor/Server/Cooker/vulkan_craft) por N segundos (padrão 10, máx 120), captura stdout/stderr em `Projects/.runs/<exe>-<ts>.log` e **mata o processo no fim** — nenhuma janela fica aberta. Devolve exit code e tail.
- `list_game_logs`: lista os logs capturados (novos primeiro).
- `read_game_log`: tail de um log capturado (`lines` para o tamanho).
- `package_game`: empacota um projeto (staging `Bin/<exe>` + `Content/` + `PackageManifest.txt` via `VulkanPackageBuilder` C++, all-or-nothing — o exe precisa já estar buildado).

Este ciclo substitui a validação visual: build → run curto → ler log, tudo pelo MCP. `start_build`/`build_status`/`cancel_build`/`list_build_jobs` formam o ciclo assíncrono com job ID e timeout.

## CLI sem cliente MCP

O `semantic-cli.mjs` expõe a **fachada semântica completa** (37 tools: projetos, cenas, entidades, componentes, scripts visuais, materiais, áudio, física, prefabs, partículas, registry/vehicle/ability/mission/world_profile/gait/simulation_lod, **run_batch transacional**) **sem servidor MCP** — mesmas factories do servidor (`callSemanticTool`), então nunca diverge da superfície MCP:

```powershell
node semantic-cli.mjs tools
node semantic-cli.mjs schema <tool>            # JSON Schema do input
node semantic-cli.mjs call <tool> '<json-args>' [--engine <root>]
```

Exit code: `0` sucesso, `2` a ferramenta retornou erro (`isError`), `3` erro de driver/uso.

O `mcp-call.mjs` inicia o servidor via stdio, chama QUALQUER ferramenta (semânticas + manutenção) e encerra:

```powershell
node mcp-call.mjs engine_overview
node mcp-call.mjs search_code "{\"query\": \"add_executable\", \"path\": \".\"}"
node mcp-call.mjs run_game "{\"exe\": \"VulkanEngineServer\", \"seconds\": 8}"
node mcp-call.mjs read_game_log "{\"log\": \"VulkanEngineServer-2026-08-14T17-00-00-000Z.log\"}"
```

Exit code: `0` sucesso, `2` a ferramenta retornou erro, `3` erro de driver.

Arquivos existentes devem ser lidos antes da edição. O hash retornado por `read_file` precisa ser enviado como `expected_sha256`; se outro agente alterar o arquivo nesse intervalo, a edição é recusada.

Diretórios de build, cache e metadados Git não aceitam escrita pelo MCP.

Projetos são armazenados em `engine/Projects/<nome>` com referência relativa `../..` para a engine. Isso permite copiar a pasta completa da engine para outro computador sem gravar caminhos absolutos da máquina original.

## Framing e recursos

- **Protocolo**: o handshake aceita **as duas versões publicadas do spec MCP** — `2024-11-05` e `2025-03-26` (default) — e devolve a versão negociada no `initialize`; versão desconhecida → erro `-32602` com `supportedProtocolVersions` completo (findings #234).
- **Framing**: o servidor aceita `Content-Length` (MCP canônico) **e** newline (linha JSON por `\n`). Frames parciais são segurados até completar o byte count declarado — sem resposta prematura nem crash (verificado por smoke dedicado). No transporte remoto, `POST /mcp` recebe JSON-RPC e responde na mesma conexão.
- **Resources**: `resources/list` expõe documentos (README, architecture, migration, pending-work, SDK manifest, dependências, determinismo, política de dependência) e os **resources dinâmicos `engine://metrics`** (métricas vivas do servidor — uptime, transport, contagem de tools, ring de auditoria, rate limit, subscriptions, SSE clients) **`engine://projects`** (lista viva de projetos, MESMA enumeração do tool `list_game_projects` — fonte única) e **por projeto via templates** (`resources/templates/list`): `engine://projects/{name}`, `.../{name}/assets`, `.../{name}/scenes` — grounded na MESMA inspeção do tool `inspect_game_project`; projeto inexistente → -32002. Todos gerados na leitura.
- **Transacional**: `run_batch` executa N operações de autoria all-or-nothing (valida tudo → aplica → reverte em falha), com `dry_run` e `update` no nível do batch.
