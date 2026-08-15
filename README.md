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

O projeto principal ainda referencia integrações promovidas mantidas localmente em
`external/solutions`. Esse catálogo é deliberadamente excluído do repositório
público porque reúne clones de terceiros e material de pesquisa, não código próprio
da engine. Portanto, esta revisão deve ser tratada como **source preview**, não como
uma distribuição autossuficiente. A migração dessas integrações para dependências
reproduzíveis e devidamente fixadas ainda está em andamento.

## Segurança

Não publique chaves, tokens, saves, logs, dumps, assets licenciados ou configurações
locais. Vulnerabilidades devem ser comunicadas pelo recurso privado de segurança do
GitHub descrito em [SECURITY.md](SECURITY.md).

## Contribuição e licença

Consulte [CONTRIBUTING.md](CONTRIBUTING.md) antes de propor alterações.

Nenhuma licença de código aberto foi concedida até o momento. O código permanece
com todos os direitos reservados ao proprietário do repositório.
