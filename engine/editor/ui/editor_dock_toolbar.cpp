#include "editor_ui_internal.h"

#include <material_graph_runtime.h>
#include <model_loader.h>
#include <texture_loader.h>

#include <editor_world.h>
#include <file_dialog/file_dialog.h>
#include <log/log.h>
#include <ui/ui_scale.h>
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
    ImGui::DockBuilderDockWindow("Viewport", centerNode);
    ImGui::DockBuilderFinish(dockspaceId);
}
}

void DrawTopToolbar(
    bool& showCameraWindow,
    bool& showAssetManagerWindow,
    bool& showInputMonitorWindow,
    bool& showSceneWindow,
    bool& showThemeWindow,
    bool& showViewportWindow,
    float effectiveUiScale
)
{
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr)
    {
        return;
    }

    const float toolbarHeight = 44.0f * effectiveUiScale;
    ImGui::SetNextWindowPos(mainViewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(mainViewport->Size.x, toolbarHeight), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainViewport->ID);
    const ImGuiWindowFlags toolbarFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("Toolbar", nullptr, toolbarFlags))
    {
        ImGui::End();
        return;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Panels");
    ImGui::SameLine();

    ImGui::BeginDisabled(showCameraWindow);
    if (ImGui::Button("Camera"))
    {
        showCameraWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showAssetManagerWindow);
    if (ImGui::Button("Assets"))
    {
        showAssetManagerWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showInputMonitorWindow);
    if (ImGui::Button("Input Monitor"))
    {
        showInputMonitorWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showSceneWindow);
    if (ImGui::Button("Scene"))
    {
        showSceneWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showThemeWindow);
    if (ImGui::Button("Theme"))
    {
        showThemeWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showViewportWindow);
    if (ImGui::Button("Viewport"))
    {
        showViewportWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Show All"))
    {
        showCameraWindow = true;
        showAssetManagerWindow = true;
        showInputMonitorWindow = true;
        showSceneWindow = true;
        showThemeWindow = true;
        showViewportWindow = true;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("Close panels with the X button and reopen them here.");

    ImGui::End();
}

ImGuiID DrawDockspaceBelowToolbar(float toolbarHeight)
{
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr)
    {
        return 0;
    }

    const ImVec2 dockspacePosition(mainViewport->Pos.x, mainViewport->Pos.y + toolbarHeight);
    const ImVec2 dockspaceSize(
        mainViewport->Size.x,
        std::max(mainViewport->Size.y - toolbarHeight, 1.0f)
    );

    ImGui::SetNextWindowPos(dockspacePosition, ImGuiCond_Always);
    ImGui::SetNextWindowSize(dockspaceSize, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainViewport->ID);

    const ImGuiWindowFlags dockspaceHostFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("EditorDockspaceHost", nullptr, dockspaceHostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), kEditorDockspaceFlags);
    EnsureDefaultDockLayout(dockspaceId, dockspaceSize);
    ImGui::End();

    return dockspaceId;
}
