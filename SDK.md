# VulkanCraft Engine — SDK redistribuível

Layout produzido por `cmake --install build --prefix <dir>` (FALTANTES item 11 /
§24 — portabilidade real). A instalação contém **somente** o necessário para
consumir a engine como SDK: headers públicos, o servidor MCP, as ferramentas
compiladas e as DLLs de runtime — sem `src/`, sem árvore de build, sem caminhos
absolutos do workspace.

```
<prefix>/
  include/engine/**        headers públicos do SDK (src/engine/public/engine/**)
  include/glm/             headers do glm (os contratos públicos incluem <glm/...>)
  lib/                     vc_sdk.lib (archive consumível) + libs estáticas das
                           dependências promovidas (zstd, blake3, flatbuffers,
                           rocksdb, recast/detour, fast-wfc, meshoptimizer,
                           xatlas) + Jolt.lib + bullet.lib
  lib/cmake/vulkan_craft_sdk/
                           config de pacote relocável (find_package CONFIG)
  bin/                     executáveis (game/editor/server/cooker/project generator/
                           package builder/shader compiler) + DLLs de runtime
  tools/mcp-server/        servidor MCP semântico (node, sem dependências externas)
  assets/                  assets compartilhados da engine
  SDK.md                   este documento
```

## Consumir o SDK

### Projeto externo via CMake (recomendado)

O SDK instala um pacote CONFIG relocável — todos os caminhos resolvem a partir
`lib/cmake/vulkan_craft_sdk/`, então o `<prefix>` pode se mover de lugar ou ser
consumido de outra máquina (mesmo OS/toolchain) sem edição:

```cmake
find_package(vulkan_craft_sdk CONFIG REQUIRED)
target_link_libraries(meu_app PRIVATE vulkan_craft_sdk)
```

O target importado expõe `include/` (engine/ + glm/) e o conjunto de link
estático completo (archive `vc_sdk` + dependências promovidas + Jolt/Bullet) —
exatamente o que `voxel_sdk_tests` linka in-tree. Configure com
`-DCMAKE_PREFIX_PATH=<prefix>`. O template `tools/external-project/` é um
consumidor mínimo desse pacote (registries + gameplay runtime de destruição);
`tools/portability/external-consumer-gate.mjs` instala, compila e roda esse
consumidor **só** contra o prefixo, sem nenhuma referência à árvore da engine.

### Direto (sem CMake)

- **C++**: `-I<prefix>/include` dá acesso aos contratos públicos
  (`engine/voxel/IVoxelWorld.hpp`, `engine/registry/*`, `engine/procgen/*`,
  `engine/gameplay/IGameplayRuntime.hpp`, `engine/world/IWorldManager.hpp`, ...)
  e linke `<prefix>/lib/vc_sdk.lib` mais as libs de `lib/`.
- **MCP**: `node <prefix>/tools/mcp-server/server.mjs` — o servidor resolve a
  raiz da engine **relativamente** (`SERVER_DIR/../..`), então funciona da
  instalação copiada para qualquer computador.

## Notas honestas

- A engine-fonte completa (para recompilar) é a árvore de trabalho: `src/`,
  `third_party/`, `tools/`, `tests/`, `CMakeLists.txt` e apenas os subdiretórios
  de `external/solutions/` consumidos pelo build (zstd, blake3, flatbuffers,
  rocksdb, entt, recast-navigation, fast-wfc, delaunator-cpp, earcut-hpp,
  meshoptimizer, xatlas) — ~215 MB, não os 11 GB do catálogo completo. O gate
  `tools/portability/clean-machine-gate.mjs` copia exatamente essa árvore
  mínima e a compila de um diretório arbitrário.
- Projetos gerados referenciam a engine **relativamente** (`../..` para
  `engine/Projects/<nome>`); o `ProjectGenerator` C++ também grava o caminho
  relativo no `project.json`.
- O SDK consumível é um **archive estático** (`vc_sdk.lib`): os TUs internos
  já vêm compilados dentro dele, então o consumidor não recompila a engine —
  mas também não herda os `-D` de compilação (ex.: `VULKANCRAFT_SOURCE_DIR`
  fica embutido como valor de runtime). O conjunto de link completo é o mesmo
  de `voxel_sdk_tests`; o target importado do pacote já lista tudo.
- Construir a engine exige toolchain + Vulkan SDK + rede (FetchContent baixa
  glfw/glm/vk-bootstrap/vma/miniaudio/imgui na primeira configuração).
