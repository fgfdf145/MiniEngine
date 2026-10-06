#include "preferences_window.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/graphics_debug_panel.h>
#include <engine/editor/ui/panels/theme_panel.h>

#include <imgui.h>

namespace me
{

PreferencesWindow::PreferencesWindow()
    : EditorWindow("preferences", "Preferences")
{
}

void PreferencesWindow::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::SetNextWindowSize(ImVec2(420.0f * UiScale(), 0.0f), ImGuiCond_FirstUseEver);
}

void PreferencesWindow::OnGui(EditorContext& context)
{
    EngineAudioSettings& audio = context.state.audio;
    ImGui::SeparatorText("Interface");
    // The same setting as the Camera panel's, saved with the editor settings.
    if (DragFloatInRange("UI Scale Multiplier", &context.style.UiScaleMultiplier(), 0.75f, 2.50f, "%.2f x"))
    {
        context.style.ApplyUiScale();
    }
    ImGui::Text("Effective UI Scale: %.2f x", context.style.EffectiveUiScale());
    ImGui::SeparatorText("Theme");
    ImGui::TextUnformatted("The editor's colours are edited in the Theme window.");
    if (ImGui::Button("Open Theme"))
    {
        context.windows.Open<ThemePanel>();
    }
    ImGui::SeparatorText("Audio");
    float volumePercent = audio.masterVolume * 100.0f;
    ImGui::BeginDisabled(audio.muted);
    if (DragFloatInRange("Master Volume", &volumePercent, 0.0f, 100.0f, "%.0f %%"))
    {
        audio.masterVolume = volumePercent / 100.0f;
    }
    ImGui::EndDisabled();
    ImGui::Checkbox("Mute", &audio.muted);
    ImGui::TextDisabled("Output: %s", context.state.audioStatus.empty() ? "None" : context.state.audioStatus.c_str());
    ImGui::SeparatorText("Rendering");
    ImGui::TextUnformatted("Debug views, tone mapping and the render passes' switches are in");
    ImGui::TextUnformatted("the Graphics Debug window.");
    if (ImGui::Button("Open Graphics Debug"))
    {
        context.windows.Open<GraphicsDebugPanel>();
    }
}
}
