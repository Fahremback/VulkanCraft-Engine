#pragma once

// ===========================================================================
// ForgeWidgets — reusable editor widgets on top of ImGui, matching the
// ForgeTheme design system. Keeps EditorApplication free of one-off style
// pushes: components become sectionHeader + vec3Property + propertyLabel.
// ===========================================================================

#include <imgui.h>
#include <string>

#include "../engine/scene/Scene.hpp"
#include "../engine/scene/Entity.hpp"
#include "../engine/core/uuid/UUID.hpp"

namespace Engine::UI {

// Collapsing header styled as a component card (accent icon + title).
// Returns true while the body should be drawn.
bool sectionHeader(const char* icon, const char* title, bool defaultOpen = true);

// Muted, left-aligned property label.
void propertyLabel(const char* label);

// Responsive property row: when the panel is wide (> 360 px) renders
// "Label | Control" in a 2-column table; when narrow, stacks the label above
// the control so text is never clipped. Returns true when a table is open
// (pass the result to endPropertyRow). Use with SetNextItemWidth(-FLT_MIN).
bool beginPropertyRow(const char* label);
void endPropertyRow(bool table);

// Label + X/Y/Z drag row with the classic red/green/blue component letters.
// Returns true when any axis changed.
bool vec3Property(const char* label, float* values, float speed = 0.1f);

// Accent-colored primary button and green success button.
bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool successButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Square icon button with a tooltip; `selected` fills it with the accent.
bool iconButton(const char* icon, const char* tooltip, bool selected = false);

// Checkbox styled for compact rows, with an optional tooltip.
bool toggle(const char* id, bool* value, const char* tooltip = nullptr);

// Rounded card container (child window).
void beginCard(const char* id);
void endCard();

// Icon + muted text hint row.
void hint(const char* icon, const char* text);

// Glyph for an entity based on its components (Scene panel icons).
const char* entityIcon(Scene* scene, const UUID& id);

} // namespace Engine::UI
