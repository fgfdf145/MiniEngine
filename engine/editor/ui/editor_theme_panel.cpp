#include <engine/editor/editor_ui.h>
#include "editor_ui_internal.h"

#include <imgui.h>

#include <array>

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
        // Palette entries are mostly translucent white overlays; drawing the swatch over the real window background
        // (no checkerboard) shows the color as it actually appears in the editor.
        changed |= ImGui::ColorEdit4(entries[index].label,
                                     &style.Colors[entries[index].colorId].x,
                                     ImGuiColorEditFlags_AlphaNoBg | ImGuiColorEditFlags_AlphaBar);
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

constexpr std::array<ThemeColorEntry, 9> kThemeTabDockEntries = {{{"Tab", ImGuiCol_Tab},
                                                                  {"Tab Hovered", ImGuiCol_TabHovered},
                                                                  {"Tab Selected", ImGuiCol_TabSelected},
                                                                  {"Tab Selected Overline", ImGuiCol_TabSelectedOverline},
                                                                  {"Tab Dimmed", ImGuiCol_TabDimmed},
                                                                  {"Tab Dimmed Selected", ImGuiCol_TabDimmedSelected},
                                                                  {"Tab Dimmed Selected Overline", ImGuiCol_TabDimmedSelectedOverline},
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

constexpr std::array<ThemeColorEntry, 8> kThemeFeedbackEntries = {{{"Text Selection", ImGuiCol_TextSelectedBg},
                                                                   {"Text Link", ImGuiCol_TextLink},
                                                                   {"Input Text Cursor", ImGuiCol_InputTextCursor},
                                                                   {"Drag Drop Target", ImGuiCol_DragDropTarget},
                                                                   {"Navigation Cursor", ImGuiCol_NavCursor},
                                                                   {"Navigation Highlight", ImGuiCol_NavWindowingHighlight},
                                                                   {"Modal Dim Background", ImGuiCol_ModalWindowDimBg},
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
