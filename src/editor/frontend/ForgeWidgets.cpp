#include "ForgeWidgets.hpp"
#include "ForgeTheme.hpp"

#include "FontAwesomeV6.h"
#include "IconsFontAwesome6.h"

namespace Engine::UI {

bool sectionHeader(const char* icon, const char* title, bool defaultOpen) {
    std::string label = std::string(icon) + "  " + title;
    ImGui::PushStyleColor(ImGuiCol_Header, Colors::AccentSoft);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.24f, 0.32f, 0.46f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.28f, 0.38f, 0.56f, 1.0f));
    const bool open = ImGui::CollapsingHeader(label.c_str(),
                                              defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleColor(3);
    return open;
}

void propertyLabel(const char* label) {
    ImGui::TextDisabled("%s", label);
}

bool beginPropertyRow(const char* label) {
    const float available = ImGui::GetContentRegionAvail().x;
    if (available >= 360.0f) {
        // Wide panel: "Label | Control" on a single row. The table id is
        // derived from the label so multiple rows in the same window do not
        // share ImGui table state.
        const std::string tableId = std::string("##PropertyRow_") + (label ? label : "");
        ImGui::BeginTable(tableId.c_str(), 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 135.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        return true;
    }
    // Narrow panel: label stacked above the control, nothing gets clipped.
    ImGui::TextUnformatted(label);
    return false;
}

void endPropertyRow(bool table) {
    if (table) ImGui::EndTable();
}

bool vec3Property(const char* label, float* values, float speed) {
    propertyLabel(label);
    bool changed = false;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float letterW = ImGui::CalcTextSize("X").x;
    const float fieldW = std::max(1.0f, (avail - 3.0f * spacing - 3.0f * letterW) / 3.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.26f, 0.26f, 1.0f));
    ImGui::Text("X");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fieldW);
    changed |= ImGui::DragFloat((std::string("##") + label + "X").c_str(), &values[0], speed);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 0.82f, 0.60f, 1.0f));
    ImGui::Text("Y");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fieldW);
    changed |= ImGui::DragFloat((std::string("##") + label + "Y").c_str(), &values[1], speed);

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.23f, 0.55f, 0.98f, 1.0f));
    ImGui::Text("Z");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fieldW);
    changed |= ImGui::DragFloat((std::string("##") + label + "Z").c_str(), &values[2], speed);
    return changed;
}

bool primaryButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, Colors::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::AccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Colors::AccentHover);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool successButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, Colors::Success);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.75f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.55f, 0.28f, 1.0f));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool iconButton(const char* icon, const char* tooltip, bool selected) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::AccentHover);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        const bool clicked = ImGui::Button(icon, ImVec2(26, 26));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered() && tooltip) ImGui::SetTooltip("%s", tooltip);
        return clicked;
    }
    const bool clicked = ImGui::Button(icon, ImVec2(26, 26));
    if (ImGui::IsItemHovered() && tooltip) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool toggle(const char* id, bool* value, const char* tooltip) {
    const bool changed = ImGui::Checkbox(id, value);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return changed;
}

void beginCard(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Colors::Surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild(id, ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void endCard() {
    ImGui::EndChild();
}

void hint(const char* icon, const char* text) {
    ImGui::TextDisabled("%s %s", icon, text);
}

const char* entityIcon(Scene* scene, const UUID& id) {
    if (!scene) return ICON_FA_CIRCLE;
    if (scene->cameraComponents.contains(id)) return ICON_FA_CAMERA;
    if (scene->lightComponents.contains(id)) return ICON_FA_SUN;
    if (scene->voxelVolumeComponents.contains(id)) return ICON_FA_CUBES;
    if (scene->meshRendererComponents.contains(id)) return ICON_FA_CUBE;
    if (scene->rigidbodyComponents.contains(id)) return ICON_FA_CUBE;
    if (scene->particleEmitterComponents.contains(id)) return ICON_FA_FIRE;
    if (scene->weaponComponents.contains(id)) return ICON_FA_GUN;
    return ICON_FA_CIRCLE;
}

} // namespace Engine::UI
