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
    ImGui::BulletText("ImGui composite: submitted");
    ImGui::SeparatorText(tr("Streaming", "Streaming"));
    ImGui::Text("Asset registry: %zu", m_assetRegistry.snapshot().size());
    ImGui::Text("Thumbnail queue: %zu", m_thumbnailQueue.size());
    ImGui::Text("Hot reload: %s", m_assetHotReload ? "active" : "inactive");
    ImGui::End();
}

} // namespace Engine
