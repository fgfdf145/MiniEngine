#include "editor_modal.h"

namespace me
{

void EditorModal::Open()
{
    m_openRequested = true;
}

bool EditorModal::ShouldDraw(const EditorContext& context) const
{
    static_cast<void>(context);
    return m_openRequested || IsOpen();
}

void EditorModal::CloseModal()
{
    ImGui::CloseCurrentPopup();
}

bool EditorModal::BeginWindow(EditorContext& context)
{
    const char* name = GetImGuiName(context);
    if (m_openRequested)
    {
        ImGui::OpenPopup(name);
        m_openRequested = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const bool open = ImGui::BeginPopupModal(name, nullptr, GetWindowFlags(context));
    // Closed by its buttons last frame, or dismissed: the manager sees the change and calls OnClose.
    *OpenFlag() = open;
    return open;
}

void EditorModal::EndWindow(bool contentsDrawn)
{
    if (contentsDrawn)
    {
        ImGui::EndPopup();
    }
}
}
