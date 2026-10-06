#pragma once

#include <imgui.h>

namespace me::ui_colors
{
// The Claude desktop app's status colours (its --cds-* design tokens, dark theme), for the few places
// that colour text or a button by meaning. The rest of the palette is ImGui's style, set in
// ConfigureImGuiStyle (engine/renderer/vulkan/imgui_layer.cpp).
inline constexpr ImVec4 kTextDanger = ImVec4(236.0f / 255.0f, 126.0f / 255.0f, 126.0f / 255.0f, 1.0f);  // --cds-text-danger
inline constexpr ImVec4 kTextWarning = ImVec4(219.0f / 255.0f, 147.0f / 255.0f, 0.0f, 1.0f);            // --cds-text-warning
inline constexpr ImVec4 kTextAccent = ImVec4(109.0f / 255.0f, 167.0f / 255.0f, 236.0f / 255.0f, 1.0f);  // --cds-text-accent
inline constexpr ImVec4 kTextSecondary = ImVec4(195.0f / 255.0f, 194.0f / 255.0f, 183.0f / 255.0f, 1.0f); // --cds-text-secondary
inline constexpr ImVec4 kFillDanger = ImVec4(208.0f / 255.0f, 59.0f / 255.0f, 59.0f / 255.0f, 1.0f);    // --cds-fill-danger
inline constexpr ImVec4 kFillDangerHover = ImVec4(227.0f / 255.0f, 73.0f / 255.0f, 72.0f / 255.0f, 1.0f); // --cds-fill-danger-hover
}
