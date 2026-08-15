# VulkanCraft Engine MCP

Servidor MCP local e portátil, sem dependências externas, para agentes de IA criarem jogos usando os recursos públicos da VulkanCraft Engine. A manutenção da própria engine também está disponível, com proteção contra edições concorrentes.

## Executar

```powershell
node C:\Users\fahre\.gemini\antigravity\scratch\vulkan_craft\engine\tools\mcp-server\server.mjs
```

## Configuração MCP

```json
{
  "mcpServers": {
    "vulkancraft-engine": {
      "command": "node",
      "args": [
        "C:\\Users\\fahre\\.gemini\\antigravity\\scratch\\vulkan_craft\\engine\\tools\\mcp-server\\server.mjs"
      ]
    }
  }
}
```

## Ferramentas

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
- `validate_game_project`: validação sem compilar ou modificar a engine.

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

Este ciclo substitui a validação visual: build → run curto → ler log, tudo pelo MCP.

## CLI sem cliente MCP

O `mcp-call.mjs` inicia o servidor via stdio, chama uma ferramenta e encerra:

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
