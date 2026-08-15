# Contribuindo

## Princípios

- Mantenha engine e jogo separados.
- Não introduza regras específicas de um jogo nos módulos públicos da engine.
- Preserve APIs existentes ou documente explicitamente a quebra.
- Inclua testes para correções e novos comportamentos.
- Nunca versione builds, caches, dumps, credenciais ou conteúdo sem licença compatível.

## Fluxo recomendado

1. Abra uma issue descrevendo problema, escopo e critério de aceite.
2. Crie uma branch curta a partir de `main`.
3. Faça commits pequenos e objetivos.
4. Execute os testes relacionados.
5. Abra um pull request explicando comportamento, validação e riscos.

## Código

- C++20 para código nativo.
- Evite dependências de plataforma nas APIs públicas.
- Prefira ownership explícito, RAII e falhas verificáveis.
- Mudanças no renderer devem respeitar sincronização, lifetime e validação Vulkan.
- Sistemas assíncronos devem definir claramente thread safety e cancelamento.
