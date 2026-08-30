#include "EditorApplication.hpp"
#include "frontend/ForgeTheme.hpp"
#include "frontend/IconsFontAwesome6.h"
#include <imgui.h>
#include <algorithm>
#include <sstream>

namespace Engine {

void EditorApplication::draw_onboarding_overlay() {
    if (!m_showOnboardingOverlay || !m_onboardingTour) return;
    const auto state = m_onboardingTour->snapshot();
    if (state.state != engine::editor::TourState::Running || state.total == 0) return;
    const std::uint64_t index = state.cursor > 0 ? state.cursor - 1 : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(390.0f, 156.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - 18.0f,
                                   vp->WorkPos.y + 18.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, UI::Colors::SurfaceAlt);
    if (ImGui::Begin("##OnboardingTour", &m_showOnboardingOverlay,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(UI::Colors::Accent, "%s  %llu/%llu",
                           state.currentStep.c_str(),
                           static_cast<unsigned long long>(index + 1),
                           static_cast<unsigned long long>(state.total));
        ImGui::Separator();
        ImGui::TextWrapped("Siga a etapa atual do tour no editor.");
        ImGui::TextDisabled("Etapa: %s", state.currentStep.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Skip")) m_onboardingTour->skip();
        ImGui::SameLine();
        if (ImGui::Button(index + 1 == state.total ? "Finish" : "Next")) {
            if (index + 1 == state.total) m_onboardingTour->complete();
            else m_onboardingTour->next();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void EditorApplication::draw_layout_settings_panel() {
    ImGui::Begin(tr("Layout Settings", "Layout Settings"), &m_showLayoutSettings);
    bool viewportFirst = m_layoutModel.viewport_first();
    if (ImGui::Checkbox(tr("Viewport primeiro", "Viewport first"), &viewportFirst)) {
        m_layoutModel.set_viewport_first(viewportFirst);
        save_layout_settings();
    }
    ImGui::SeparatorText(tr("Painéis", "Panels"));
    for (const auto& panel : m_panelRegistry.panels()) {
        bool visible = m_layoutModel.is_visible(panel.id);
        if (ImGui::Checkbox(panel.title.c_str(), &visible)) {
            m_layoutModel.set_visible(panel.id, visible);
            if (panel.id == "hierarchy") m_showHierarchy = visible;
            if (panel.id == "inspector") m_showInspector = visible;
            if (panel.id == "viewport") m_showViewport = visible;
            if (panel.id == "content_browser") m_showContentBrowser = visible;
            if (panel.id == "console") m_showConsole = visible;
            if (panel.id == "render_debugger") m_showRenderDebugger = visible;
            if (panel.id == "script_debugger") m_showScriptDebugger = visible;
            if (panel.id == "script_canvas") m_showScriptCanvas = visible;
            if (panel.id == "ai_debugger") m_showAiDebug = visible;
            save_layout_settings();
        }
    }
    ImGui::Separator();
    if (ImGui::Button(tr("Restaurar padrão", "Reset to defaults"))) {
        apply_layout_defaults();
        save_layout_settings();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Salvar agora", "Save now"))) save_layout_settings();
    ImGui::End();
}

void EditorApplication::draw_render_debugger_panel() {
    // Feed the debug view from the LIVE editor state before drawing, so the
    // on-screen panel always reflects the current probe/card/capture state
    // (the same data the diagnostics command serializes).
    feed_render_debug_view();
    ImGui::Begin(tr("Render Debugger", "Render Debugger"), &m_showRenderDebugger);
    ImGui::TextColored(UI::Colors::Accent, "%s", tr("Diagnóstico do frame ao vivo", "Live frame diagnostics"));
    ImGui::Separator();
    ImGui::Text("FPS: %.1f", m_fps);
    ImGui::Text("Frame: %.2f ms", m_frameTimeMs);
    ImGui::Text("RAM: %.1f MB", m_ramUsageMb);
    ImGui::Text("Viewport: %u x %u", m_offscreen.width, m_offscreen.height);
    ImGui::Text("Swapchain: %u x %u", m_swapchainExtent.width, m_swapchainExtent.height);
    ImGui::Text("MSAA: %u", static_cast<unsigned>(m_viewportSamples));
    ImGui::SeparatorText(tr("Passes", "Passes"));
    ImGui::BulletText("Shadow map: %s", m_shadowMap.enabled ? "ready" : "disabled");
    ImGui::BulletText("Scene: %s", m_offscreen.framebuffer != VK_NULL_HANDLE ? "recorded" : "waiting");
    // Real per-pass timings recorded by render_scene_to_offscreen through the
    // public IRenderPassMetrics consumer (agente 4 — Aceleração §C/D). This
    // panel renders the SAME data the diagnostics command exposes via
    // GET /render-diagnostics, so the on-screen debugger is a real consumer
    // of the rendering telemetry contract.
    if (m_renderMetrics) {
        const auto snapshot = m_renderMetrics->snapshot();
        if (!snapshot.passes.empty()) {
            ImGui::SeparatorText(tr("Tempos reais", "Real pass timings"));
            for (const auto& pass : snapshot.passes) {
                ImGui::BulletText("%s: cpu %.3f ms avg (p95 %.3f) x %zu",
                                  pass.name.c_str(), pass.cpuMsAvg, pass.cpuMsP95,
                                  pass.samples);
            }
        } else {
            ImGui::TextDisabled("%s", tr("Sem passes registrados ainda", "No passes recorded yet"));
        }
        if (!snapshot.pools.empty()) {
            ImGui::SeparatorText(tr("Memória", "Memory"));
            for (const auto& pool : snapshot.pools) {
                ImGui::BulletText("%s: %llu B (pico %llu)", pool.name.c_str(),
                                  static_cast<unsigned long long>(pool.currentBytes),
                                  static_cast<unsigned long long>(pool.peakBytes));
            }
        }
    }
    // Debug-overlay snapshot (C-block): the SAME IRenderingDebugView the
    // diagnostics command serializes, so the on-screen panel reflects real
    // probe / card / capture state fed from the live scene.
    if (m_renderDebugView) {
        const auto dbg = m_renderDebugView->snapshot();
        ImGui::SeparatorText(tr("Debug Overlays", "Debug Overlays"));
        ImGui::BulletText("Probes: %u (pending %u, sunRev %u)", dbg.probeCount,
                          dbg.pendingProbes, dbg.sunRevision);
        ImGui::BulletText("Cards: %u (captured %u, pending %u)", dbg.cardCount,
                          dbg.capturedCount, dbg.pendingCount);
        ImGui::BulletText("Trace paths: %zu", dbg.tracePaths.size());
        ImGui::BulletText("Disoccluded px: %u, denoiser conf: %u",
                          dbg.disoccludedPixels, dbg.confidenceLevel);
    }
    // Frame-graph overlay: the REAL compiled viewport render graph the
    // executor records each frame (same contract the game's frame uses) —
    // pass names in compiled order plus live executor counters.
    {
        const auto& compiled = m_viewportRenderGraphExecutor.compile_result();
        ImGui::SeparatorText(tr("Frame Graph", "Frame Graph"));
        if (!compiled.order.empty()) {
            for (const auto passId : compiled.order) {
                const auto* desc = m_viewportRenderGraph.pass(passId);
                ImGui::BulletText("%s", desc ? desc->name.c_str() : "?");
            }
        } else {
            ImGui::TextDisabled("%s", tr("Nenhum grafo compilado ainda", "No graph compiled yet"));
        }
        ImGui::BulletText("Barriers: %zu (executados %llu)", compiled.barriers.size(),
                          static_cast<unsigned long long>(m_viewportRenderGraphExecutor.total_barriers()));
        ImGui::BulletText("Passes executados: %zu", m_viewportRenderGraphExecutor.executed_pass_count());
    }
    ImGui::SeparatorText(tr("Streaming", "Streaming"));
    ImGui::Text("Asset registry: %zu", m_assetRegistry.snapshot().size());
    ImGui::Text("Thumbnail queue: %zu", m_thumbnailQueue.size());
    ImGui::Text("Hot reload: %s", m_assetHotReload ? "active" : "inactive");
    ImGui::End();
}

void EditorApplication::draw_ai_debug_panel() {
    ImGui::Begin(tr("IA Debugger", "AI Debugger"), &m_showAiDebug);
    const PlayState state = m_playMode.get_state();
    const bool playing =
        (state == PlayState::Play || state == PlayState::Simulate);
    if (!playing) {
        // Explicit absent state: the consumer is only fed while play mode runs.
        ImGui::TextColored(UI::Colors::Accent, "%s",
                           tr("Sem modo play ativo", "No active play mode"));
        ImGui::TextDisabled("%s", tr("Inicie play mode para alimentar o snapshot de IA.",
                                     "Start play mode to feed the AI snapshot."));
        ImGui::End();
        return;
    }
    if (!m_playAiRecorder || m_playNavAgents.empty()) {
        // Absent state: no live AI agent to snapshot — explicit, not empty UI.
        ImGui::TextColored(UI::Colors::Warning, "%s",
                           tr("Nenhum agente de navegação vivo", "No live navigation agent"));
        ImGui::TextDisabled("%s", tr("Adicione uma entidade com NavigationComponent.",
                                     "Add an entity with a NavigationComponent."));
        ImGui::End();
        return;
    }
    ImGui::Text("%s: %zu", tr("Agentes vivos", "Live agents"), m_playAiAgentCount);
    // Entity selection: list all live nav agents; clicking one refocuses the
    // recorder (the snapshot valid for the selected entity is shown).
    ImGui::SeparatorText(tr("Entidade focada", "Focused entity"));
    {
        const Engine::UUID focusId = m_playAiFocus;
        const auto focusIt = m_playNavAgents.find(focusId);
        const bool focusValid = focusIt != m_playNavAgents.end();
        ImGui::Text("%s %s", tr("Foco:", "Focus:"),
                    focusValid ? focusId.to_string().c_str()
                               : tr("(auto: primeiro vivo)", "(auto: first live)"));
        if (ImGui::BeginCombo(tr("Selecionar entidade", "Select entity"),
                              focusValid ? focusId.to_string().c_str()
                                         : tr("Auto", "Auto"))) {
            for (const auto& [id, agent] : m_playNavAgents) {
                (void)agent;
                const bool selected = (m_playAiFocus == id);
                if (ImGui::Selectable(id.to_string().c_str(), selected)) {
                    m_playAiFocus = id;
                }
            }
            ImGui::EndCombo();
        }
        if (m_playAiFocus.is_valid() && !focusValid) {
            ImGui::TextDisabled("%s", tr("Entidade selecionada não está viva; usando auto.",
                                         "Selected entity not live; using auto."));
        }
    }
    ImGui::SeparatorText(tr("Snapshot", "Snapshot"));
    if (m_playAiNodes.empty() && m_playAiBlackboard.empty()) {
        // Absent state: recorder exists but no tick was fed for the focused
        // entity — explicit, never a silent empty box.
        ImGui::TextDisabled("%s", tr("Sem snapshot para a entidade focada neste frame.",
                                     "No snapshot for the focused entity this frame."));
    }
    for (const auto& node : m_playAiNodes) {
        const char* colorMarker = node.status == "succeeded" ? "[OK]"
                                  : (node.status == "running" ? "[>>]"
                                  : (node.status == "failed" ? "[!!]" : "[--]"));
        ImGui::BulletText("%s %s%s%s", colorMarker, std::string(static_cast<size_t>(node.depth) * 2u, ' ').c_str(),
                          node.id.c_str(), node.detail.empty() ? "" : (" — " + node.detail).c_str());
    }
    if (!m_playAiBlackboard.empty()) {
        ImGui::SeparatorText(tr("Blackboard", "Blackboard"));
        for (const auto& entry : m_playAiBlackboard) {
            ImGui::BulletText("%s = %s", entry.key.c_str(), entry.value.c_str());
        }
    }
    ImGui::TextDisabled("%s: %s", tr("JSON", "JSON"), m_playAiDebugJson.c_str());
    ImGui::End();
}

} // namespace Engine
