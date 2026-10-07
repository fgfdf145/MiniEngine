#pragma once

#include <engine/editor/engine_settings.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>

namespace me
{

// The editor's look: the UI scale (the window's DPI times the user's multiplier) and the colour
// palette the Theme panel edits. The ImGui style is rebuilt from a base style captured on the first
// frame, so scaling never compounds and the palette survives a scale change.
class EditorStyle
{
  public:
    // Captures the base style and the built-in palette on the first call. Call each frame before
    // ApplySettings and ApplyUiScale.
    void BeginFrame(SDL_Window* window);

    // The saved scale and colours; once the first frame has captured the style.
    void ApplySettings(const EditorUiSettings& settings);
    void WriteSettings(EditorUiSettings& settings) const;

    // The user's multiplier, 0.75 to 2.5; ApplyUiScale makes a change take effect.
    float& UiScaleMultiplier()
    {
        return m_uiScale;
    }
    float EffectiveUiScale() const
    {
        return m_effectiveUiScale;
    }
    // The window's own DPI scale, before the user's multiplier.
    float WindowDpiScale() const;
    void ApplyUiScale();

    // The palette as it was when last captured, which Reset Theme Colors goes back to.
    void CaptureDefaultThemeColors();
    void ResetThemeColorsToDefault();
    // After a colour is edited in the live style: keeps it when the style is next rebuilt.
    void SyncBaseStyleColorsFromCurrentStyle();

  private:
    SDL_Window* m_window = nullptr;
    float m_uiScale = 1.0f;
    float m_effectiveUiScale = 1.0f;
    ImGuiStyle m_baseStyle{};
    std::array<ImVec4, ImGuiCol_COUNT> m_defaultThemeColors{};
    // The palette as ConfigureImGuiStyle set it; the settings file keeps only the colours that differ.
    std::array<ImVec4, ImGuiCol_COUNT> m_builtInThemeColors{};
    bool m_hasCapturedBaseStyle = false;
    bool m_hasCapturedDefaultThemeColors = false;
};
}
