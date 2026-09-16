#pragma once

// ===========================================================================
// ForgeTheme — the editor design system (premium dark).
//
// Deep charcoal surfaces with real contrast between background / surface /
// input / button / tab / top bar, one blue accent, subtle borders, generous
// spacing. Engine behaviour is untouched — this only restyles the ImGui
// presentation layer (see PORTS.md for the frontend port).
// ===========================================================================

#include <imgui.h>

namespace Engine::UI {

struct Colors {
    // Low-contrast layered surfaces instead of the old grey "desktop widget"
    // look.  The spacing/shape system carries hierarchy; borders are only a
    // subtle separator, not a box around every control.
    static constexpr ImVec4 Background    { 0.045f, 0.052f, 0.068f, 1.0f };
    static constexpr ImVec4 Surface       { 0.068f, 0.078f, 0.102f, 1.0f };
    static constexpr ImVec4 SurfaceAlt    { 0.092f, 0.105f, 0.138f, 1.0f };
    static constexpr ImVec4 Border        { 0.145f, 0.165f, 0.210f, 1.0f };

    static constexpr ImVec4 Text          { 0.92f, 0.94f, 0.98f, 1.0f };
    static constexpr ImVec4 TextSecondary { 0.70f, 0.74f, 0.82f, 1.0f };
    static constexpr ImVec4 TextMuted     { 0.52f, 0.56f, 0.64f, 1.0f };

    static constexpr ImVec4 Accent        { 0.39f, 0.43f, 1.00f, 1.0f };
    static constexpr ImVec4 AccentHover   { 0.47f, 0.52f, 1.00f, 1.0f };
    static constexpr ImVec4 AccentSoft    { 0.145f, 0.165f, 0.285f, 1.0f };

    static constexpr ImVec4 Success       { 0.17f, 0.72f, 0.40f, 1.0f };
    static constexpr ImVec4 Warning       { 0.92f, 0.63f, 0.15f, 1.0f };
    static constexpr ImVec4 Danger        { 0.90f, 0.28f, 0.30f, 1.0f };
};

// Applies the full Forge dark theme to the current ImGui context.
void applyForgeTheme();

} // namespace Engine::UI
