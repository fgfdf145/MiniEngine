#include "scene_reset_modal.h"

#include <engine/editor/editor_ui.h>

#include <imgui.h>

namespace me
{

SceneResetModal::SceneResetModal()
    : EditorModal("scene_reset", "Discard Scene Contents?")
{
}

void SceneResetModal::Ask(Reset reset)
{
    m_pending = reset;
    Open();
}

void SceneResetModal::OnClose(EditorContext& context)
{
    static_cast<void>(context);
    m_pending.reset();
}

void SceneResetModal::OnGui(EditorContext& context)
{
    const bool newScene = m_pending == Reset::New;
    if (newScene)
    {
        ImGui::TextUnformatted("Start a new scene? Every entity is removed and the scene is no longer tied to its file;");
        ImGui::TextUnformatted("a sun and the default atmosphere are added.");
    }
    else
    {
        ImGui::TextUnformatted("Remove every entity from the scene? The environment and the scene's file are kept.");
    }
    ImGui::TextUnformatted("Changes not saved to the scene file are lost. This cannot be undone.");
    ImGui::Separator();

    if (ImGui::Button(newScene ? "New Scene" : "Clear Scene", ImVec2(120.0f * UiScale(), 0.0f)))
    {
        context.result.actions.newScene = newScene;
        context.result.actions.clearScene = !newScene;
        m_pending.reset();
        CloseModal();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f * UiScale(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_pending.reset();
        CloseModal();
    }
}
}
