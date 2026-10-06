#include "editor_style.h"

#include <engine/platform/ui/ui_scale.h>

#include <cmath>

namespace me
{

void EditorStyle::BeginFrame(SDL_Window* window)
{
    m_window = window;
    if (!m_hasCapturedBaseStyle)
    {
        m_baseStyle = ImGui::GetStyle();
        m_hasCapturedBaseStyle = true;
    }
    if (!m_hasCapturedDefaultThemeColors)
    {
        CaptureDefaultThemeColors();
        m_builtInThemeColors = m_defaultThemeColors;
        m_hasCapturedDefaultThemeColors = true;
    }
}

void EditorStyle::ApplySettings(const EditorUiSettings& settings)
{
    m_uiScale = platform::ui::ResolveConfiguredUiScale(settings.scale);
    if (settings.theme.hasCustomColors)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
        {
            if (!settings.theme.colorDefined[static_cast<size_t>(colorIndex)])
            {
                continue;
            }
            style.Colors[colorIndex] = settings.theme.colors[static_cast<size_t>(colorIndex)];
        }
        SyncBaseStyleColorsFromCurrentStyle();
    }
}

void EditorStyle::WriteSettings(EditorUiSettings& settings) const
{
    platform::ui::SetConfiguredUiScaleForCurrentPlatform(settings.scale, m_uiScale);
    if (!m_hasCapturedDefaultThemeColors)
    {
        return; // no frame yet: the palette was never applied, so the loaded overrides stand
    }
    settings.theme.hasCustomColors = false;

    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        const ImVec4& color = style.Colors[colorIndex];
        const ImVec4& builtIn = m_builtInThemeColors[static_cast<size_t>(colorIndex)];
        const bool changed = color.x != builtIn.x || color.y != builtIn.y || color.z != builtIn.z || color.w != builtIn.w;
        settings.theme.colors[static_cast<size_t>(colorIndex)] = color;
        settings.theme.colorDefined[static_cast<size_t>(colorIndex)] = changed;
        settings.theme.hasCustomColors |= changed;
    }
}

float EditorStyle::WindowDpiScale() const
{
    return platform::ui::ResolveWindowUiScale(m_window);
}

void EditorStyle::ApplyUiScale()
{
    ImGuiStyle& style = ImGui::GetStyle();
    m_effectiveUiScale = platform::ui::ClampUiScale(WindowDpiScale() * m_uiScale);

    if (std::abs(style.FontScaleMain - m_effectiveUiScale) <= 0.001f)
    {
        return;
    }

    style = m_baseStyle;
    style.ScaleAllSizes(m_effectiveUiScale);
    style.FontScaleMain = m_effectiveUiScale;
}

void EditorStyle::CaptureDefaultThemeColors()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        m_defaultThemeColors[static_cast<size_t>(colorIndex)] = style.Colors[colorIndex];
    }
}

void EditorStyle::SyncBaseStyleColorsFromCurrentStyle()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        m_baseStyle.Colors[colorIndex] = style.Colors[colorIndex];
    }
}

void EditorStyle::ResetThemeColorsToDefault()
{
    ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        style.Colors[colorIndex] = m_defaultThemeColors[static_cast<size_t>(colorIndex)];
    }
    SyncBaseStyleColorsFromCurrentStyle();
}
}
