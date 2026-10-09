#include "unsaved_changes_modal.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui_colors.h>
#include <engine/logic/editor_world.h>

#include <imgui.h>

#include <filesystem>

namespace me
{

UnsavedChangesModal::UnsavedChangesModal()
    : EditorModal("unsaved_changes", "Unsaved Changes")
{
}

void UnsavedChangesModal::Ask(Then then, std::string scenePath)
{
    m_then = then;
    m_scenePath = std::move(scenePath);
    m_saving = false;
    m_saveError.clear();
    Open();
}

void UnsavedChangesModal::OnClose(EditorContext& context)
{
    static_cast<void>(context);
    m_then.reset();
    m_saving = false;
}

void UnsavedChangesModal::Continue(EditorContext& context)
{
    EditorUiActions& actions = context.result.actions;
    actions.discardUnsavedChanges = true;
    switch (*m_then)
    {
    case Then::Quit:
        actions.quitConfirmed = true;
        break;
    case Then::OpenScene:
        actions.selectedSceneLoadPath = m_scenePath;
        break;
    case Then::NewScene:
        actions.newScene = true;
        break;
    }
    m_then.reset();
    CloseModal();
}

void UnsavedChangesModal::OnGui(EditorContext& context)
{
    if (!m_then.has_value())
    {
        CloseModal();
        return;
    }
    // The save asked for last frame has run: go on, or say why it did not.
    if (m_saving)
    {
        m_saving = false;
        if (context.frame.lastSceneIoError.empty())
        {
            Continue(context);
            return;
        }
        m_saveError = context.frame.lastSceneIoError;
    }

    const std::string& path = context.scene.GetSceneFilePath();
    const std::string name = path.empty() ? std::string("The new scene") : "'" + std::filesystem::path(path).filename().string() + "'";
    ImGui::Text("%s has unsaved changes.", name.c_str());
    const char* what = *m_then == Then::Quit ? "Save them before closing the editor?"
                       : *m_then == Then::OpenScene ? "Save them before opening another scene?"
                                                    : "Save them before starting a new scene?";
    ImGui::TextUnformatted(what);
    if (!m_saveError.empty())
    {
        ImGui::TextColored(ui_colors::kTextDanger, "Not saved: %s", m_saveError.c_str());
    }
    ImGui::Separator();

    const ImVec2 button(120.0f * UiScale(), 0.0f);
    const bool save = ImGui::Button("Save", button);
    // A scene with no file yet is saved where the user picks, as File > Save asks.
    std::optional<std::string> savePath;
    if (path.empty())
    {
        ImGui::PushID("unsaved_changes.save_as");
        savePath = PickFilePath(FileDialogType::SaveScene, save);
        ImGui::PopID();
    }
    else if (save)
    {
        savePath = path;
    }
    if (savePath.has_value())
    {
        context.result.actions.selectedSceneSavePath = *savePath;
        m_saving = true;
        m_saveError.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save", button))
    {
        Continue(context);
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_then.reset();
        CloseModal();
    }
}
}
