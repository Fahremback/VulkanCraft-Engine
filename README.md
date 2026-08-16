# VulkanCraft Engine

Engine experimental de jogos 3D escrita em C++20, com renderização Vulkan e uma
arquitetura modular orientada à criação de mundos voxel, ferramentas de autoria e
simulações em larga escala.

> **Estado:** desenvolvimento ativo. Este repositório público contém somente o
> código-fonte da engine. O jogo utilizado durante o desenvolvimento, builds,
> caches, assets privados e o catálogo local de projetos de referência não fazem
> parte desta publicação.

## Objetivos

- Renderer Vulkan com materiais, sombras, pós-processamento e ferramentas de shader.
- Editor visual e modo de execução de projetos.
- ECS, cena, componentes, prefabs, reflexão e serialização.
- Física, animação, áudio espacial, navegação e sistemas de gameplay reutilizáveis.
- Runtime voxel com chunks, geração procedural, streaming, LOD e persistência.
- SDK e servidor MCP para automação e autoria assistida por IA.

## Estrutura

```text
src/app/       executáveis e ciclo de vida
src/editor/    editor e ferramentas de autoria
src/engine/    módulos públicos da engine
src/features/  plugins e recursos opcionais
shaders/       shaders ativos
tests/         testes e benchmarks
tools/         ferramentas de build e servidor MCP
third_party/   dependências vendorizadas necessárias
```

## Build

Requisitos básicos:

- Windows 10/11 x64;
- CMake 3.20 ou superior;
- compilador C++20 (Visual Studio 2022 recomendado);
- Vulkan SDK.

O clone público requer 11 bibliotecas fixadas em `external/solutions`: Zstandard,
BLAKE3, FlatBuffers, RocksDB, EnTT, Recast/Detour, Fast-WFC, Delaunator, Earcut,
meshoptimizer e xatlas. Elas não são os 97 projetos do catálogo interno; somente
essas 11 participam do build atual. Versões, URLs, caminhos e comandos
reproduzíveis estão em [DEPENDENCIES.md](DEPENDENCIES.md).

O CMake baixa GLFW, GLM, vk-bootstrap, Vulkan Memory Allocator, miniaudio e ImGui
automaticamente. Bullet e Jolt já estão vendorizados em `third_party`.

Enquanto essas integrações não forem migradas para `FetchContent` ou
um gerenciador de pacotes, esta revisão deve ser tratada como **source preview**,
não como uma distribuição autossuficiente. Se alguma delas estiver ausente, o
CMake encerra imediatamente e aponta para a documentação.

## Segurança

Não publique chaves, tokens, saves, logs, dumps, assets licenciados ou configurações
locais. Vulnerabilidades devem ser comunicadas pelo recurso privado de segurança do
GitHub descrito em [SECURITY.md](SECURITY.md).

## Contribuição e licença

Consulte [CONTRIBUTING.md](CONTRIBUTING.md) antes de propor alterações.

Nenhuma licença de código aberto foi concedida até o momento. O código permanece
com todos os direitos reservados ao proprietário do repositório.
