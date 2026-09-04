#include <engine/editor/editor_ui.h>
#include "editor_ui_internal.h"

#include <engine/asset/material_graph_runtime.h>
#include <engine/asset/model_loader.h>
#include <engine/asset/texture_loader.h>

#include <engine/logic/editor_world.h>
#include <engine/platform/file_dialog/file_dialog.h>
#include <engine/core/log/log.h>
#include <engine/platform/ui/ui_scale.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <yaml-cpp/yaml.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace me
{

namespace
{
struct ThemeColorEntry
{
    const char* label = "";
    ImGuiCol colorId = ImGuiCol_Text;
};

bool DrawThemeColorSection(const char* title, const ThemeColorEntry* entries, size_t entryCount)
{
    if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen))
    {
        return false;
    }

    bool changed = false;
    ImGuiStyle& style = ImGui::GetStyle();
    for (size_t index = 0; index < entryCount; ++index)
    {
        changed |= ImGui::ColorEdit4(entries[index].label, &style.Colors[entries[index].colorId].x);
    }
    return changed;
}

constexpr std::array<ThemeColorEntry, 8> kThemeSurfaceEntries = {{{"Window Background", ImGuiCol_WindowBg},
                                                                  {"Child Background", ImGuiCol_ChildBg},
                                                                  {"Popup Background", ImGuiCol_PopupBg},
                                                                  {"Frame Background", ImGuiCol_FrameBg},
                                                                  {"Frame Hovered", ImGuiCol_FrameBgHovered},
                                                                  {"Frame Active", ImGuiCol_FrameBgActive},
                                                                  {"Menu Bar", ImGuiCol_MenuBarBg},
                                                                  {"Scrollbar Background", ImGuiCol_ScrollbarBg}}};

constexpr std::array<ThemeColorEntry, 10> kThemeControlEntries = {{{"Button", ImGuiCol_Button},
                                                                   {"Button Hovered", ImGuiCol_ButtonHovered},
                                                                   {"Button Active", ImGuiCol_ButtonActive},
                                                                   {"Header", ImGuiCol_Header},
                                                                   {"Header Hovered", ImGuiCol_HeaderHovered},
                                                                   {"Header Active", ImGuiCol_HeaderActive},
                                                                   {"Check Mark", ImGuiCol_CheckMark},
                                                                   {"Slider Grab", ImGuiCol_SliderGrab},
                                                                   {"Slider Grab Active", ImGuiCol_SliderGrabActive},
                                                                   {"Separator", ImGuiCol_Separator}}};

constexpr std::array<ThemeColorEntry, 7> kThemeChromeEntries = {{{"Title Background", ImGuiCol_TitleBg},
                                                                 {"Title Active", ImGuiCol_TitleBgActive},
                                                                 {"Title Collapsed", ImGuiCol_TitleBgCollapsed},
                                                                 {"Border", ImGuiCol_Border},
                                                                 {"Resize Grip", ImGuiCol_ResizeGrip},
                                                                 {"Resize Grip Hovered", ImGuiCol_ResizeGripHovered},
                                                                 {"Resize Grip Active", ImGuiCol_ResizeGripActive}}};

constexpr std::array<ThemeColorEntry, 7> kThemeTabDockEntries = {{{"Tab", ImGuiCol_Tab},
                                                                  {"Tab Hovered", ImGuiCol_TabHovered},
                                                                  {"Tab Active", ImGuiCol_TabActive},
                                                                  {"Tab Unfocused", ImGuiCol_TabUnfocused},
                                                                  {"Tab Unfocused Active", ImGuiCol_TabUnfocusedActive},
                                                                  {"Docking Preview", ImGuiCol_DockingPreview},
                                                                  {"Docking Empty Background", ImGuiCol_DockingEmptyBg}}};

constexpr std::array<ThemeColorEntry, 9> kThemeStateEntries = {{{"Text", ImGuiCol_Text},
                                                                {"Text Disabled", ImGuiCol_TextDisabled},
                                                                {"Scrollbar Grab", ImGuiCol_ScrollbarGrab},
                                                                {"Scrollbar Grab Hovered", ImGuiCol_ScrollbarGrabHovered},
                                                                {"Scrollbar Grab Active", ImGuiCol_ScrollbarGrabActive},
                                                                {"Table Header", ImGuiCol_TableHeaderBg},
                                                                {"Table Border Strong", ImGuiCol_TableBorderStrong},
                                                                {"Table Border Light", ImGuiCol_TableBorderLight},
                                                                {"Table Row Alt", ImGuiCol_TableRowBgAlt}}};

constexpr std::array<ThemeColorEntry, 5> kThemeFeedbackEntries = {{{"Text Selection", ImGuiCol_TextSelectedBg},
                                                                   {"Drag Drop Target", ImGuiCol_DragDropTarget},
                                                                   {"Navigation Cursor", ImGuiCol_NavCursor},
                                                                   {"Navigation Highlight", ImGuiCol_NavWindowingHighlight},
                                                                   {"Separator Hovered", ImGuiCol_SeparatorHovered}}};

constexpr std::array<ThemeColorEntry, 1> kThemeFeedbackActiveEntries = {{{"Separator Active", ImGuiCol_SeparatorActive}}};
}

void EditorUiController::CaptureDefaultThemeColors()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        m_defaultThemeColors[static_cast<size_t>(colorIndex)] = style.Colors[colorIndex];
    }
}

void EditorUiController::SyncBaseStyleColorsFromCurrentStyle()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        m_baseStyle.Colors[colorIndex] = style.Colors[colorIndex];
    }
}

void EditorUiController::ResetThemeColorsToDefault()
{
    ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        style.Colors[colorIndex] = m_defaultThemeColors[static_cast<size_t>(colorIndex)];
    }
    SyncBaseStyleColorsFromCurrentStyle();
}

bool EditorUiController::DrawThemeEditorWindow()
{
    if (!ImGui::Begin("Theme", &m_showThemeWindow))
    {
        ImGui::End();
        return false;
    }

    ImGui::TextWrapped("Edit the editor palette live. Changes apply immediately and remain stable when UI scale changes.");
    ImGui::Separator();

    bool changed = false;
    if (ImGui::Button("Reset Theme Colors"))
    {
        ResetThemeColorsToDefault();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Re-capture Current As Default"))
    {
        CaptureDefaultThemeColors();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Tip: right-click a color swatch for copy/paste or manual hex input.");

    changed |= DrawThemeColorSection("Surfaces", kThemeSurfaceEntries.data(), kThemeSurfaceEntries.size());
    changed |= DrawThemeColorSection("Controls", kThemeControlEntries.data(), kThemeControlEntries.size());
    changed |= DrawThemeColorSection("Chrome", kThemeChromeEntries.data(), kThemeChromeEntries.size());
    changed |= DrawThemeColorSection("Tabs And Docking", kThemeTabDockEntries.data(), kThemeTabDockEntries.size());
    changed |= DrawThemeColorSection("States", kThemeStateEntries.data(), kThemeStateEntries.size());
    changed |= DrawThemeColorSection("Feedback", kThemeFeedbackEntries.data(), kThemeFeedbackEntries.size());
    changed |= DrawThemeColorSection("Feedback Active", kThemeFeedbackActiveEntries.data(), kThemeFeedbackActiveEntries.size());

    if (changed)
    {
        SyncBaseStyleColorsFromCurrentStyle();
    }

    ImGui::End();
    return changed;
}
}
