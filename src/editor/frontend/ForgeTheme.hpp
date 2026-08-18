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
    static constexpr ImVec4 Background    { 0.06f, 0.07f, 0.09f, 1.0f };
    static constexpr ImVec4 Surface       { 0.10f, 0.11f, 0.14f, 1.0f };
    static constexpr ImVec4 SurfaceAlt    { 0.14f, 0.15f, 0.19f, 1.0f };
    static constexpr ImVec4 Border        { 0.22f, 0.24f, 0.30f, 1.0f };

    static constexpr ImVec4 Text          { 0.92f, 0.94f, 0.98f, 1.0f };
    static constexpr ImVec4 TextSecondary { 0.70f, 0.74f, 0.82f, 1.0f };
    static constexpr ImVec4 TextMuted     { 0.52f, 0.56f, 0.64f, 1.0f };

    static constexpr ImVec4 Accent        { 0.32f, 0.55f, 1.00f, 1.0f };
    static constexpr ImVec4 AccentHover   { 0.40f, 0.62f, 1.00f, 1.0f };
    static constexpr ImVec4 AccentSoft    { 0.17f, 0.23f, 0.34f, 1.0f };

    static constexpr ImVec4 Success       { 0.17f, 0.72f, 0.40f, 1.0f };
    static constexpr ImVec4 Warning       { 0.92f, 0.63f, 0.15f, 1.0f };
    static constexpr ImVec4 Danger        { 0.90f, 0.28f, 0.30f, 1.0f };
};

// Applies the full Forge dark theme to the current ImGui context.
void applyForgeTheme();

} // namespace Engine::UI
