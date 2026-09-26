#include "editor_ui_internal.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace me
{

namespace
{
constexpr ImGuiDockNodeFlags kEditorDockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;

void EnsureDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& dockspaceSize)
{
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockspaceId);
    if (dockNode != nullptr &&
        (dockNode->ChildNodes[0] != nullptr || dockNode->ChildNodes[1] != nullptr || dockNode->Windows.Size > 0))
    {
        return;
    }

    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr)
    {
        return;
    }

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, kEditorDockspaceFlags | ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

    ImGuiID leftNode = 0;
    ImGuiID rightNode = 0;
    ImGuiID centerNode = dockspaceId;
    ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Left, 0.28f, &leftNode, &centerNode);
    ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Right, 0.24f, &rightNode, &centerNode);

    ImGuiID lowerRightNode = 0;
    ImGuiID upperRightNode = rightNode;
    ImGui::DockBuilderSplitNode(upperRightNode, ImGuiDir_Down, 0.42f, &lowerRightNode, &upperRightNode);

    ImGui::DockBuilderDockWindow("Scene", leftNode);
    ImGui::DockBuilderDockWindow("Camera", lowerRightNode);
    ImGui::DockBuilderDockWindow("Graphics Debug", lowerRightNode);
    ImGui::DockBuilderDockWindow("Viewport", centerNode);
    ImGui::DockBuilderFinish(dockspaceId);
}
}

ImGuiID DrawEditorDockspace(bool resetLayout)
{
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr)
    {
        return 0;
    }

    // Fills the viewport's work area: what the main menu bar and the toolbar leave.
    const ImGuiID dockspaceId = ImHashStr("EditorDockspace");
    if (resetLayout)
    {
        // Undocks every window; EnsureDefaultDockLayout then docks them as on first run.
        ImGui::DockBuilderRemoveNode(dockspaceId);
    }
    ImGui::DockSpaceOverViewport(dockspaceId, mainViewport, kEditorDockspaceFlags);
    EnsureDefaultDockLayout(dockspaceId, mainViewport->WorkSize);
    return dockspaceId;
}
}
