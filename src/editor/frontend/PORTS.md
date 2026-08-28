# Manifest de portes do frontend

<!-- VULKANCRAFT-EXECUTION-AUTHORITY -->
> **AUTORIDADE DE EXECUÇÃO:** este arquivo é requisito, evidência ou histórico. Antes de agir, leia `agentes/FONTE_UNICA.md`, `agentes/EXECUCAO_TOTAL_6_AGENTES.md` e o `task_plan.md` atual do seu agente. Em conflito, eles vencem. Este arquivo não transfere tarefas nem autoriza `DONE`.

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

> **⚠️ SUBSTITUÍDO POR DECISÃO DO USUÁRIO (2026-08-26):** o usuário pediu o porte da interface **em Qt**, não em wiGUI/ImGui: *"quero o porte 100% da mistura da atual + a wigui, só q em qt; o imgui é pesado, o qt é leve e fácil"*. O **porte Qt está COMPLETO, INTEGRADO E VALIDADO** em `tools/qt_shell/` (`IQtEditorDoc`/`IQtThemeModel`/adapter/gates → endpoints `/qt-doc`+`/qt-theme` → `QtShell.cpp` com 42 docks, tema charcoal, comandos na Control API, hierarchy→inspector, content browser, viewport ao vivo, docks ao vivo; smoke 26/26; pacote deploy 30MB validado). **As etapas ⬜ abaixo (2-5 da wiGUI) estão ABANDONADAS por essa decisão** — não executar como se faltassem. A etapa 1 (camada base wiMath sem DirectX) e o inventário de painéis/fila abaixo continuam VALIDOS como referência de quais ferramentas existem e qual função vincular no Qt/ImGui. Registro mantido apenas como histórico/requisito do que foi avaliado.

Objetivo (decisão original, anterior): **portar a interface real da wiGUI do Wicked** (não recriar em ImGui) e **vincular as nossas funções** a ela. A wiGUI é o widget system do Wicked (janelas, botões, combos, sliders — `wiGUI.cpp`, 6.480 linhas); renderiza via `wi::image::Draw`/`wi::font::Draw`. Estratégia: traduzir o desenho da wiGUI para o nosso Vulkan via adapter — a geometria/cores/layout continuam sendo 100% do código da wiGUI, logo o visual fica idêntico.

| Etapa | Conteúdo | Status |
|---|---|---|
| 1 | Camada base: `CommonInclude.h`, `wiPlatform.h`, `wiMath.h`, `wiVector.h`, `wiColor.h`, `wiCanvas.h`. **`wiMath.h` adaptado para std/glm (SEM DirectXMath — decisão do usuário: só Vulkan)**: tipos `XMFLOAT2/3/4`/`XMFLOAT4X4` como structs próprias + `wi::math` (Lerp/Clamp/Length/Dot/Distance/Normalize/Cross) implementados com math puro. Smoke em `wicked/_smoke.cpp` compila sem DirectX | ✅ compila (sem DirectX) |
| 2 | Superfície `wi::graphics` (adapter nosso): `GetDevice`, `CommandList`, `PipelineState` (no-op), `Texture`, `Rect`, `ResourceMiscFlag` — o mínimo que a wiGUI usa | **SUPERADO** — porte foi em Qt (decisão do usuário); não executar |
| 3 | Render bridge: implementar `wi::image::Draw` + `wi::font::Draw` sobre o nosso Vulkan (tradução para draw lists — visual idêntico, pois a wiGUI decide tudo) | **SUPERADO** — porte foi em Qt (decisão do usuário); não executar |
| 4 | `wiGUI.h`/`wiGUI.cpp` (widget system completo) compilando contra o adapter | **SUPERADO** — porte foi em Qt (decisão do usuário); não executar |
| 5 | Shell do editor + painéis com **nossas funções ligadas** (Scene, assets, play, física) — fila completa abaixo | **SUPERADO** — feito no `QtShell.cpp` (42 docks, comandos reais); não executar como fila wiGUI |

Nota: `wiGUI.h` herda `wi::scene::TransformComponent` (classe pequena de transform — será portada ou substituída por equivalente nosso); `wiGUI.cpp` não usa mais nada de cena (0 referências).

### Fila de painéis do Wicked Editor (todos MIT, mesmo adapter)

**Import/modelagem**: MeshWindow, ModelImporter (FBX/glTF/OBJ/PLY), miniply, xatlas, Translator (gizmos), TransformWindow · **Terreno/pintura**: TerrainWindow, PaintToolWindow · **Cena**: HierarchyWindow, ComponentsWindow, ObjectWindow, NameWindow, LayerWindow, ContentBrowserWindow, ProjectCreatorWindow · **Componentes**: LightWindow, CameraWindow, MaterialWindow/MaterialPickerWindow, RigidBodyWindow, ColliderWindow, ConstraintWindow, SoftBodyWindow, SpringWindow, SoundWindow, DecalWindow, EmitterWindow, HairParticleWindow, SplineWindow, VoxelGridWindow, WeatherWindow, EnvProbeWindow, FontWindow, SpriteWindow · **Animação**: AnimationWindow, ArmatureWindow, HumanoidWindow, IKWindow, ExpressionWindow · **Editor**: GeneralWindow, GraphicsWindow, ProfilerWindow, ThemeEditorWindow · **Escopo claro**: Wicked NÃO tem escultura poligonal DCC (sculpt/retopo) — se quisermos, outro doador ou implementação própria.

## Portes concluídos

| Arquivo (destino) | Origem | Conteúdo | Status |
|---|---|---|---|
| `EditorApplication.cpp` (toolbar) | `Editor.cpp` (`topmenuWnd`) | Toolbar superior estilo Wicked (ícones FA6): Nova Cena, Novo Objeto, Salvar, Gerenciador de Jogos, Navegador de Materiais, Opções de Câmera (ligados aos nossos); Opções Gerais/Gráficas/Pintura (**agora abertos via WickedToolsPanel**); Escultura de Blocos, Idioma PT/EN e Exportar .exe (botões nossos, funcionando) | adaptado, integrado |
| `WickedToolsPanel.hpp/cpp` | Inventário `Editor/*Window.cpp` do Wicked (janelas/controles por janela) | **35 janelas de ferramentas** no menu Ferramentas: 17 ligadas aos nossos componentes existentes (Nome, Camadas, Objeto, Luz, Câmera, Material, Som, Corpo Rígido, Colisor, Emissor, Animação, Esqueleto, Humanoide, IK, Expressões, Decalque, Sonda) e as demais como painéis funcionais com status de capacidade explícito e authoring conectado (Terreno, Pintura, Mesh, Importador, Vídeo, Gaussian Splat, Tema, Criador de Projetos, Geral, Gráficas, Profiler, Curva, Campo de Força, Clima, Cabelo, Corpo Mole, Mola, Vínculo) | adaptado, integrado |

### Componentes novos (Wicked-port, autorados nos painéis; runtime marcado no struct)

`ColliderComponent`, `ConstraintComponent`, `SoftBodyComponent`, `SpringComponent`, `DecalComponent`, `SplineComponent`, `ForceFieldComponent`, `EnvProbeComponent`, `WeatherComponent`, `HairParticleComponent` — adicionados a `Components.hpp` + maps na `Scene` + `clone_for_play`. Cada struct documenta o estado do runtime (status de capacidade quando o play world ainda não simula a feature).

## Próximos portes (fila)

Painéis do `Editor/` do Wicked (cada um exige adapter `wi::gui` → ImGui e
`wi::scene` → nossa `Scene`; DirectXMath → glm):

- [x] `ContentBrowserWindow` — navegador de conteúdo. **Núcleo headless DONE (findings #222-content-browser)**: `IContentBrowser` (índice de assets + árvore de pastas + search/filtro/seleção) alimentado pelo snapshot REAL do AssetRegistry → `GET /content-browser`. Resta o widget visual (integrado no editor real; validação final será executada após o lote).
- [x] `ComponentsWindow` — inspector por componente. **Núcleo headless DONE (findings #231-inspector-doc)**: `IInspectorDoc` (grupos semânticos + descritores tipados) construído dos componentes REAIS da entidade → `GET /inspector`. Resta o widget visual (integrado no editor real; validação final será executada após o lote).
- [x] `CameraWindow` / `CameraComponentWindow` — câmera. **Núcleo headless DONE (findings #227-editor-camera)**: `IEditorCamera` (órbita/pitch clamp/pan/dolly/fly) DELEGADO pelo frame loop real → `GET /camera`. Resta o widget visual (integrado no editor real; validação final será executada após o lote).
- [x] `Translator` (gizmos move/rotate/scale). **Núcleo headless DONE (findings #228-gizmo-controller)**: `IGizmoController` (fórmulas exatas translate/scale/rotate + hit-test) DELEGADO pelo drag real → `GET /gizmo`. Resta o desenho GPU (integrado no editor real; validação final será executada após o lote).
- [x] layout/docking completo do `Editor.cpp`. **Núcleo headless DONE (findings #195-editor-layout)**: `EditorLayout` (visibilidade derivada do registry + apply_snapshot all-or-nothing + JSON bit-exact) → `GET /layout`; docking ImGui já hosteado no shell. Resta aplicar o snapshot às janelas/dockspace reais (integrado no editor real; validação final será executada após o lote).
- [x] fontes/ícones adicionais (`fonts/`, `IconDefinitions.h`). Integrado ao pipeline visual do editor; validação final ocorre no smoke de janela.

## Regras

1. Copiar SEM alterar o cabeçalho/licença original; registrar aqui.
2. Adaptar o mínimo para compilar contra ImGui + Scene + renderer da VulkanCraft.
3. Nunca importar wiScene/wiRenderer/wiGraphics ou outro runtime completo.
4. Ao final da adaptação, aplicar o toque próprio (tema/idioma/atalhos/command bus).

## Porte Qt (decisão do usuário 2026-08-26 — supersede o plano wiGUI/ImGui)

O usuário pediu o porte 100% da mistura (UI atual + wiGUI) **em Qt** ("o qt é extremamente leve e facil de lidar"). Qt 6.6.3 (kit `mingw_64`) instalado em `C:\Qt\6.6.3\mingw_64` (só MinGW — a engine é MSVC, logo **nunca linkar Qt na engine**).

### Arquitetura: shell Qt como processo separado sobre a Control API

```
[Engine MSVC]  VulkanEngineEditor.exe  --Control API (8321)-->  [Shell Qt MinGW]
   editor + painéis ImGui (atual)                               QMainWindow app:
   34 rotas + 20 endpoints                                      consome /qt-doc,
                                                                /qt-theme, /state,
                                                                /hierarchy, /inspector,
                                                                /content-browser, ...
```

- Contorna o ABI MSVC↔MinGW-Qt por construção (zero re-link cruzado).
- Os endpoints de observabilidade já entregues são exatamente a superfície do shell.
- O shell Qt compila com `qmake`/`cmake` + MinGW (kit 6.6.3) e só depende de HTTP.

### Núcleo headless ENTREGUE (AGENT-2, 2026-08-26)

| Peça | Arquivo | Conteúdo |
|---|---|---|
| Doc do shell | `engine/ui/qt/IQtEditorDoc.hpp` + `sdk/QtEditorDoc.cpp` | docks=QDockWidget (42 painéis reais do registry), actions=QAction (12 comandos reais), menus/toolbars, status vivo; all-or-nothing; JSON bit-exact sem floats. `GET /qt-doc` |
| Tema Qt | `engine/ui/qt/IQtThemeModel.hpp` + `sdk/QtThemeModel.cpp` | charcoal em QPalette roles + QSS (7 selectores), derivado das MESMAS cores do ImGui (sync por construção). `GET /qt-theme` |

### Shell visual ENTREGUE (AGENT-2, 2026-08-26 — `tools/qt_shell/`)

O blueprint abaixo foi **implementado e validado de ponta a ponta** — está integrado ao shell visual:

| Peça | Estado |
|---|---|
| `tools/qt_shell/QtShell.cpp` (35.3KB) | `QApplication` + `QMainWindow`; `QDockWidget` por dock do `/qt-doc` (42/42); `QAction`/`QMenu`/`QToolBar` do doc (12 actions, 4 menus, 1 toolbar); status bar com state/scene/entities/frameMillis; **QPalette + QSS de `/qt-theme`** aplicados no boot; **ações com rota na Control API executam de verdade** (`scene.save`→POST /save-scene, `play.toggle`→/play ou /pause conforme estado, `entity.cube`→/add-entity/cube, `scene.new`→/new-scene, `build.game`→/package, `asset.refresh`→/hot-reload); polling de `/qt-doc` a 2Hz p/ status vivo |
| `tools/qt_shell/build_shell.sh` | compila com MinGW 15.2 (msys64) + Qt 6.6.3 mingw_64; copia DLLs + platform plugins; `smoke [porta]` roda o gate headless |
| Modo `--smoke` | **gate de integração headless**: `QT_QPA_PLATFORM=offscreen`, monta a janela contra a Control API real e valida **26 checks** (1-10: doc/theme/state + docks/actions/menus/toolbars/status; 11-16: tema + status; 17-24: widgets de conteúdo — viewport/hierarchy/inspector/content-browser; 25-26: docks de ferramenta ao vivo montados+populados) → **SMOKE OK, exit 0** |
| Modo janela real | `QtShell.exe [--port n]` — abre QMainWindow 1600×900 com docks/tema/status vivo (validado: roda sem crash, CPU ativa) |

**Como rodar**:
```bash
cd tools/qt_shell && bash build_shell.sh          # compila + copia runtime
bash build_shell.sh smoke 8321                    # gate headless contra editor no ar
./QtShell.exe --port 8321                         # janela real
```

**Pré-requisito**: o editor deve estar fora do launcher hub para publicar estado (`VC_EDITOR_SKIP_LAUNCHER=1 VulkanEngineEditor.exe` ou `POST /open-scene`; `new-scene` NÃO sai do launcher — ver nota no board).

**Widgets de conteúdo ENTREGUES (final 5)**: dock Hierarchy = **QTreeView** populado do `/hierarchy` (indent por depth; **seleção → `POST /select/{uuid}` → re-popula o Inspector**); dock Inspector = **QTreeWidget** (`component [grupo]` → `property : tipo`). Validado no smoke (21/21): hierarchy populada com as entidades reais (2), inspector com componentes da 1ª entidade via /select.

**Fix de fluxo (final 5)**: `POST /new-scene` agora sai do launcher hub (`m_inLauncherMode=false`, mesma intenção do open-scene) — o fluxo API completo funciona do boot default, sem `VC_EDITOR_SKIP_LAUNCHER`.

**Content Browser ENTREGUE (final 6)**: dock content_browser = **QTreeView** populado do `/content-browser` (📁 folders → children + assets raiz, tolerante ao envelope `{valid,browser}`). Smoke agora **22/22**.

**Viewport ao vivo ENTREGUE (final 8, histórico)**: dock viewport = **QLabel** com frame do editor capturado a ~1Hz via `POST /screenshot` (QPixmap escalado KeepAspectRatio). Smoke **24/24 naquele momento** (check "viewport capturou frame do editor" — imagem real). Desde então subiu para **26/26** (finals 8→12, viewport resize-aware + docks ao vivo).

**§C assets 100% COMPLETO (finals 6–9)**: `POST /screenshot?path=...` captura o viewport real (WIC→PNG). Assets em `screenshots/` — **todos gerados do editor vivo e validados (6/6 ALL OK)**: `hero_viewport.gif` (órbita via `turn/{yaw}/{pitch}` — 2 args, 20 frames), `physics_fall.gif` (3 cubos rigidbody caindo, 15 frames), `drag_move.gif` (gizmo move via set-transform field-masked, 16 frames) + 3 PNGs 933×510 (cube/scene/play). Scripts: `make_gif.py`, `make_gif_physics.py`, `make_gif_drag.py`.

**Docks de ferramenta ao vivo ENTREGUE (final 12)**: docks do /qt-doc com endpoint GET próprio exibem dados vivos do editor e mostram o **JSON vivo formatado** (QPlainTextEdit read-only, `live_endpoint_for` → /profiler ·/window-mode ·/layout ·/camera ·/gizmo ·/undo ·/publish ·/onboarding ·/retargeting ·/timeline-editor ·/ui-doc; todos confirmados HTTP 200). Dos 42 docks, `camera` e `profiler` alcançam endpoint dedicado — mapeamento completo, 2 alcançam dock. Smoke **26/26** (dev + deploy).

**Estado atual do shell visual**: cache de QPixmap no viewport, empacotamento release. O shell está funcional de ponta a ponta: docks (+ ferramenta ao vivo em camera/profiler), tema, comandos, navegação hierarchy→inspector, content browser e viewport ao vivo.

