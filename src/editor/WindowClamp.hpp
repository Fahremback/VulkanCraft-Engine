#pragma once

#include <imgui.h>

#include <algorithm>

// Keep a floating ImGui window fully inside the editor window:
//  - it can never be dragged off-screen (position is clamped every frame, so
//    dragging "blocks" at the edge before the window can leave);
//  - it can never be resized below the minimum, so text/functions stay visible.
// Docked windows are already bounded by the dockspace and are skipped.
// Call right after ImGui::Begin() for floating windows.
inline void clamp_floating_window_on_screen(float minW = 260.0f, float minH = 180.0f) {
    if (ImGui::IsWindowDocked()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 workMin = vp->WorkPos;
    const ImVec2 workMax(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);

    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 clampedSize(std::max(size.x, minW), std::max(size.y, minH));
    ImVec2 clampedPos = pos;
    clampedPos.x = std::clamp(clampedPos.x, workMin.x, std::max(workMin.x, workMax.x - clampedSize.x));
    clampedPos.y = std::clamp(clampedPos.y, workMin.y, std::max(workMin.y, workMax.y - clampedSize.y));
    // Only touch the window when it actually needs correcting, so the built-in
    // clamp/docking logic keeps working normally the rest of the time.
    if (clampedSize.x != size.x || clampedSize.y != size.y) ImGui::SetWindowSize(clampedSize);
    if (clampedPos.x != pos.x || clampedPos.y != pos.y) ImGui::SetWindowPos(clampedPos);
}
