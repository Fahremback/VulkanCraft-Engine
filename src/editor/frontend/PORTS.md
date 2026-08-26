# Manifest de portes do frontend

Registro arquivo-a-arquivo do código copiado das engines doadoras (decisão normativa:
[`docs/EDITOR_FRONTEND_WICKED_EZENGINE.md`](../../docs/EDITOR_FRONTEND_WICKED_EZENGINE.md)).
Regra: cabeçalho MIT original preservado, origem registrada aqui, adaptação mínima
aos contratos da VulkanCraft, depois o toque próprio.

## Doadores

| Doador | Repositório | Commit | Licença |
|---|---|---|---|
| Wicked Engine (Editor) | `external/solutions/wicked-engine` | `2aa9fdf89d2a5ce830dbe3709d9335f5bb89031b` (2026-08-14) | MIT, © Turánszki János |
| ezEngine (EditorFramework) | `external/solutions/ezengine` | pendente (primeiro porte) | MIT |

## Portes

| Arquivo (destino) | Origem | Conteúdo | Licença/atribuição | Status |
|---|---|---|---|---|
| `FontAwesomeV6.h` | `Editor/FontAwesomeV6.h` | Fonte Font Awesome 6 Free Solid embutida como array `uint8_t[]` (`font_awesome_v6`) | Fonte: Font Awesome Free (SIL OFL 1.1, © Fonticons, Inc.); arquivo copiado do Wicked Engine Editor (MIT) | integrado (fonte de ícones no ImGui) |
| `IconsFontAwesome6.h` | `Editor/IconsFontAwesome6.h` | Macros `ICON_FA_*` (codepoints UTF-8, faixa `0xe005`–`0xf8ff`) | Gerado por IconFontCppHeaders (MIT, juliettef) a partir do Font Awesome 6 (ícones: CC BY 4.0, © Fonticons, Inc.) | integrado (ícones da toolbar) |
| `liberation_sans.h` | `WickedEngine/Utility/liberation_sans.h` | Fonte de UI **Liberation Sans** v2.1.5 (TTF zstd-compressed, mesma fonte que o Wicked usa) | Liberation Sans (SIL OFL 1.1, © Red Hat); arquivo copiado do Wicked Engine (MIT) | integrado (fonte base do ImGui, descomprimida via `ICompressionProvider`) |
| tema charcoal | `WickedEngine/wiGUI.cpp` (cores: janelas ~60/255, painéis ~50/255, scrollbars 80/140/180) | Tema monocromático cinza do Wicked aplicado ao `ImGuiStyle` | Wicked Engine (MIT) | integrado (tema do editor) |

## Porte fiel da wiGUI (interface real do Wicked)

Objetivo (decisão do usuário): **portar a interface real da wiGUI do Wicked** (não recriar em ImGui) e **vincular as nossas funções** a ela. A wiGUI é o widget system do Wicked (janelas, botões, combos, sliders — `wiGUI.cpp`, 6.480 linhas); renderiza via `wi::image::Draw`/`wi::font::Draw`. Estratégia: traduzir o desenho da wiGUI para o nosso Vulkan via adapter — a geometria/cores/layout continuam sendo 100% do código da wiGUI, logo o visual fica idêntico.

| Etapa | Conteúdo | Status |
|---|---|---|
| 1 | Camada base: `CommonInclude.h`, `wiPlatform.h`, `wiMath.h`, `wiVector.h`, `wiColor.h`, `wiCanvas.h`. **`wiMath.h` adaptado para std/glm (SEM DirectXMath — decisão do usuário: só Vulkan)**: tipos `XMFLOAT2/3/4`/`XMFLOAT4X4` como structs próprias + `wi::math` (Lerp/Clamp/Length/Dot/Distance/Normalize/Cross) implementados com math puro. Smoke em `wicked/_smoke.cpp` compila sem DirectX | ✅ compila (sem DirectX) |
| 2 | Superfície `wi::graphics` (adapter nosso): `GetDevice`, `CommandList`, `PipelineState` (no-op), `Texture`, `Rect`, `ResourceMiscFlag` — o mínimo que a wiGUI usa | ⬜ |
| 3 | Render bridge: implementar `wi::image::Draw` + `wi::font::Draw` sobre o nosso Vulkan (tradução para draw lists — visual idêntico, pois a wiGUI decide tudo) | ⬜ |
| 4 | `wiGUI.h`/`wiGUI.cpp` (widget system completo) compilando contra o adapter | ⬜ |
| 5 | Shell do editor + painéis com **nossas funções ligadas** (Scene, assets, play, física) — fila completa abaixo | ⬜ |

Nota: `wiGUI.h` herda `wi::scene::TransformComponent` (classe pequena de transform — será portada ou substituída por equivalente nosso); `wiGUI.cpp` não usa mais nada de cena (0 referências).

### Fila de painéis do Wicked Editor (todos MIT, mesmo adapter)

**Import/modelagem**: MeshWindow, ModelImporter (FBX/glTF/OBJ/PLY), miniply, xatlas, Translator (gizmos), TransformWindow · **Terreno/pintura**: TerrainWindow, PaintToolWindow · **Cena**: HierarchyWindow, ComponentsWindow, ObjectWindow, NameWindow, LayerWindow, ContentBrowserWindow, ProjectCreatorWindow · **Componentes**: LightWindow, CameraWindow, MaterialWindow/MaterialPickerWindow, RigidBodyWindow, ColliderWindow, ConstraintWindow, SoftBodyWindow, SpringWindow, SoundWindow, DecalWindow, EmitterWindow, HairParticleWindow, SplineWindow, VoxelGridWindow, WeatherWindow, EnvProbeWindow, FontWindow, SpriteWindow · **Animação**: AnimationWindow, ArmatureWindow, HumanoidWindow, IKWindow, ExpressionWindow · **Editor**: GeneralWindow, GraphicsWindow, ProfilerWindow, ThemeEditorWindow · **Escopo claro**: Wicked NÃO tem escultura poligonal DCC (sculpt/retopo) — se quisermos, outro doador ou implementação própria.

## Portes concluídos

| Arquivo (destino) | Origem | Conteúdo | Status |
|---|---|---|---|
| `EditorApplication.cpp` (toolbar) | `Editor.cpp` (`topmenuWnd`) | Toolbar superior estilo Wicked (ícones FA6): Nova Cena, Novo Objeto, Salvar, Gerenciador de Jogos, Navegador de Materiais, Opções de Câmera (ligados aos nossos); Opções Gerais/Gráficas/Pintura (**agora abertos via WickedToolsPanel**); Escultura de Blocos, Idioma PT/EN e Exportar .exe (botões nossos, funcionando) | adaptado, integrado |
| `WickedToolsPanel.hpp/cpp` | Inventário `Editor/*Window.cpp` do Wicked (janelas/controles por janela) | **35 janelas de ferramentas** no menu Ferramentas: 17 ligadas aos nossos componentes existentes (Nome, Camadas, Objeto, Luz, Câmera, Material, Som, Corpo Rígido, Colisor, Emissor, Animação, Esqueleto, Humanoide, IK, Expressões, Decalque, Sonda) e as demais como painéis funcionais com `TODO(frontend-port)` explícito (Terreno, Pintura, Mesh, Importador, Vídeo, Gaussian Splat, Tema, Criador de Projetos, Geral, Gráficas, Profiler, Curva, Campo de Força, Clima, Cabelo, Corpo Mole, Mola, Vínculo) | adaptado, integrado |

### Componentes novos (Wicked-port, autorados nos painéis; runtime marcado no struct)

`ColliderComponent`, `ConstraintComponent`, `SoftBodyComponent`, `SpringComponent`, `DecalComponent`, `SplineComponent`, `ForceFieldComponent`, `EnvProbeComponent`, `WeatherComponent`, `HairParticleComponent` — adicionados a `Components.hpp` + maps na `Scene` + `clone_for_play`. Cada struct documenta o estado do runtime (`TODO(frontend-port)` quando o play world ainda não simula a feature).

## Próximos portes (fila)

Painéis do `Editor/` do Wicked (cada um exige adapter `wi::gui` → ImGui e
`wi::scene` → nossa `Scene`; DirectXMath → glm):

- [ ] `ContentBrowserWindow` — navegador de conteúdo. **Núcleo headless DONE (findings #222-content-browser)**: `IContentBrowser` (índice de assets + árvore de pastas + search/filtro/seleção) alimentado pelo snapshot REAL do AssetRegistry → `GET /content-browser`. Resta o widget visual (HUMAN-VISUAL).
- [ ] `ComponentsWindow` — inspector por componente. **Núcleo headless DONE (findings #231-inspector-doc)**: `IInspectorDoc` (grupos semânticos + descritores tipados) construído dos componentes REAIS da entidade → `GET /inspector`. Resta o widget visual (HUMAN-VISUAL).
- [ ] `CameraWindow` / `CameraComponentWindow` — câmera. **Núcleo headless DONE (findings #227-editor-camera)**: `IEditorCamera` (órbita/pitch clamp/pan/dolly/fly) DELEGADO pelo frame loop real → `GET /camera`. Resta o widget visual (HUMAN-VISUAL).
- [ ] `Translator` (gizmos move/rotate/scale). **Núcleo headless DONE (findings #228-gizmo-controller)**: `IGizmoController` (fórmulas exatas translate/scale/rotate + hit-test) DELEGADO pelo drag real → `GET /gizmo`. Resta o desenho GPU (HUMAN-VISUAL).
- [ ] layout/docking completo do `Editor.cpp`. **Núcleo headless DONE (findings #195-editor-layout)**: `EditorLayout` (visibilidade derivada do registry + apply_snapshot all-or-nothing + JSON bit-exact) → `GET /layout`; docking ImGui já hosteado no shell. Resta aplicar o snapshot às janelas/dockspace reais (HUMAN-VISUAL).
- [ ] fontes/ícones adicionais (`fonts/`, `IconDefinitions.h`). Sem núcleo headless (assets binários/GPU) — HUMAN-VISUAL.

## Regras

1. Copiar SEM alterar o cabeçalho/licença original; registrar aqui.
2. Adaptar o mínimo para compilar contra ImGui + Scene + renderer da VulkanCraft.
3. Nunca importar wiScene/wiRenderer/wiGraphics ou outro runtime completo.
4. Ao final da adaptação, aplicar o toque próprio (tema/idioma/atalhos/command bus).
