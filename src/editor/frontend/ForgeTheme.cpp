#include "ForgeTheme.hpp"

namespace Engine::UI {

void applyForgeTheme() {
    ImGui::StyleColorsDark();

    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry — roomier, modern spacing.
    s.WindowPadding    = ImVec2(12, 12);
    s.FramePadding     = ImVec2(10, 7);
    s.CellPadding      = ImVec2(8, 6);
    s.ItemSpacing      = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(6, 5);
    s.IndentSpacing    = 18.0f;
    s.ScrollbarSize    = 11.0f;
    s.GrabMinSize      = 9.0f;

    // Rounded, modern corners.
    s.WindowRounding    = 8.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 12.0f;
    s.GrabRounding      = 6.0f;
    s.TabRounding       = 6.0f;
    // Docked-window tab bars use the FittingPolicyMixed policy: tabs shrink to
    // TabMinWidthShrink before scrolling. FLT_MAX means "never shrink", so tab
    // titles keep their natural width and the bar scrolls horizontally instead
    // of squashing labels.
    s.TabMinWidthShrink = FLT_MAX;

    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;

    // Keep the global minimum small. A large WindowMinSize also clamps
    // fixed-size shell windows: the app bar's 56 px request would be forced
    // up to the minimum (ImGui does ImMax(requested, WindowMinSize)). Panels
    // that need a real minimum get one via SetNextWindowSizeConstraints() at
    // their own Begin() call instead.
    s.WindowMinSize = ImVec2(32.0f, 32.0f);

    auto& c = s.Colors;

    c[ImGuiCol_Text]                 = Colors::Text;
    c[ImGuiCol_TextDisabled]         = Colors::TextMuted;

    c[ImGuiCol_WindowBg]             = Colors::Background;
    c[ImGuiCol_ChildBg]              = Colors::Surface;
    c[ImGuiCol_PopupBg]              = Colors::Surface;

    c[ImGuiCol_Border]               = Colors::Border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]              = Colors::SurfaceAlt;
    c[ImGuiCol_FrameBgHovered]       = Colors::AccentSoft;
    c[ImGuiCol_FrameBgActive]        = Colors::AccentSoft;

    c[ImGuiCol_TitleBg]              = Colors::Surface;
    c[ImGuiCol_TitleBgActive]        = Colors::SurfaceAlt;
    c[ImGuiCol_TitleBgCollapsed]     = Colors::Surface;

    c[ImGuiCol_MenuBarBg]            = Colors::Surface;

    c[ImGuiCol_Button]               = Colors::SurfaceAlt;
    c[ImGuiCol_ButtonHovered]        = Colors::AccentSoft;
    c[ImGuiCol_ButtonActive]         = ImVec4(0.20f, 0.28f, 0.42f, 1.0f);

    c[ImGuiCol_Header]               = Colors::SurfaceAlt;
    c[ImGuiCol_HeaderHovered]        = Colors::AccentSoft;
    c[ImGuiCol_HeaderActive]         = Colors::Accent;

    c[ImGuiCol_CheckMark]            = Colors::Accent;
    c[ImGuiCol_SliderGrab]           = Colors::Accent;
    c[ImGuiCol_SliderGrabActive]     = Colors::AccentHover;

    c[ImGuiCol_Tab]                  = Colors::Surface;
    c[ImGuiCol_TabHovered]           = Colors::SurfaceAlt;
    c[ImGuiCol_TabActive]            = Colors::AccentSoft;
    c[ImGuiCol_TabUnfocused]         = Colors::Surface;
    c[ImGuiCol_TabUnfocusedActive]   = Colors::SurfaceAlt;

    c[ImGuiCol_Separator]            = Colors::Border;
    c[ImGuiCol_SeparatorHovered]     = Colors::Accent;
    c[ImGuiCol_SeparatorActive]      = Colors::AccentHover;

    c[ImGuiCol_DockingPreview]       = ImVec4(Colors::Accent.x, Colors::Accent.y,
                                              Colors::Accent.z, 0.25f);
    c[ImGuiCol_DockingEmptyBg]       = Colors::Background;

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.35f, 0.38f, 0.45f, 0.8f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45f, 0.50f, 0.58f, 0.9f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.52f, 0.57f, 0.66f, 1.0f);

    c[ImGuiCol_PlotLines]            = Colors::Accent;
    c[ImGuiCol_PlotLinesHovered]     = Colors::AccentHover;
    c[ImGuiCol_PlotHistogram]        = Colors::Accent;
    c[ImGuiCol_PlotHistogramHovered] = Colors::AccentHover;

    // Text selection matches the accent.
    c[ImGuiCol_TextSelectedBg]       = ImVec4(Colors::Accent.x, Colors::Accent.y,
                                              Colors::Accent.z, 0.25f);

    // Modal/child dimming stays subtle on a dark theme.
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.02f, 0.03f, 0.05f, 0.45f);
}

} // namespace Engine::UI
