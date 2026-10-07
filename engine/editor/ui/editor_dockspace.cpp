#include "editor_ui_internal.h"
#include "framework/editor_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace me
{

namespace
{
constexpr ImGuiDockNodeFlags kEditorDockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;

void EnsureDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& dockspaceSize, std::span<EditorPanel* const> panels)
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

    // Each panel names its place; the upper right is left for whatever the user docks there.
    for (const EditorPanel* panel : panels)
    {
        ImGuiID node = 0;
        switch (panel->GetDefaultDockSlot())
        {
        case EditorDockSlot::Floating:
            continue;
        case EditorDockSlot::Left:
            node = leftNode;
            break;
        case EditorDockSlot::Center:
            node = centerNode;
            break;
        case EditorDockSlot::RightBottom:
            node = lowerRightNode;
            break;
        }
        ImGui::DockBuilderDockWindow(panel->GetTitle().c_str(), node);
    }
    ImGui::DockBuilderFinish(dockspaceId);
}

// Auto Layout leaves every panel next to the viewport at least this wide and tall (points at UI scale 1).
constexpr float kAutoLayoutMinPanelPoints = 160.0f;

// The smallest a node can be made along `axis`: each of its panels at the minimum, side by side where
// it is split along the axis.
float MinNodeExtent(const ImGuiDockNode* node, ImGuiAxis axis, float minPanel, float spacing)
{
    if (node->IsLeafNode())
    {
        return minPanel;
    }
    const ImGuiDockNode* first = node->ChildNodes[0];
    const ImGuiDockNode* second = node->ChildNodes[1];
    if (!first->IsVisible || !second->IsVisible)
    {
        return MinNodeExtent(first->IsVisible ? first : second, axis, minPanel, spacing);
    }
    const float firstMin = MinNodeExtent(first, axis, minPanel, spacing);
    const float secondMin = MinNodeExtent(second, axis, minPanel, spacing);
    return node->SplitAxis == axis ? firstMin + secondMin + spacing : std::max(firstMin, secondMin);
}

// A split along one axis above the viewport's dock node: the side the viewport is in, the other side,
// and how much the other side can give before its panels reach the minimum.
struct ViewportSplit
{
    ImGuiDockNode* own = nullptr;
    ImGuiDockNode* other = nullptr;
    float slack = 0.0f;
};

// Nearest first.
std::vector<ViewportSplit> CollectViewportSplits(ImGuiDockNode* leaf, ImGuiAxis axis, float minPanel, float spacing)
{
    std::vector<ViewportSplit> splits;
    for (ImGuiDockNode* child = leaf; child->ParentNode != nullptr; child = child->ParentNode)
    {
        ImGuiDockNode* parent = child->ParentNode;
        ImGuiDockNode* other = parent->ChildNodes[0] == child ? parent->ChildNodes[1] : parent->ChildNodes[0];
        if (parent->SplitAxis != axis || other == nullptr || !other->IsVisible)
        {
            continue;
        }
        splits.push_back({child, other, std::max(other->Size[axis] - MinNodeExtent(other, axis, minPanel, spacing), 0.0f)});
    }
    return splits;
}

// Moves the splits so `leaf` is `size` along `axis`. Growing takes from the other sides by how much
// each can give; shrinking gives to them by their sizes. Each own side is locked at its new size for
// ImGui's next layout pass (DockNodeTreeUpdatePosSize), which also keeps it as the node's saved size.
void ResizeAlongAxis(ImGuiDockNode* leaf, const std::vector<ViewportSplit>& splits, ImGuiAxis axis, float size)
{
    const float delta = size - leaf->Size[axis];
    if (splits.empty() || std::abs(delta) < 0.5f)
    {
        return;
    }
    std::vector<float> weights;
    float total = 0.0f;
    size_t heaviest = 0;
    for (const ViewportSplit& split : splits)
    {
        weights.push_back(delta > 0.0f ? split.slack : split.other->Size[axis]);
        total += weights.back();
        heaviest = weights.back() > weights[heaviest] ? weights.size() - 1 : heaviest;
    }
    if (total <= 0.0f)
    {
        return;
    }
    // Whole points from each other side; what the rounding leaves goes to the one that gives the most.
    std::vector<float> given(splits.size());
    float assigned = 0.0f;
    for (size_t i = 0; i < splits.size(); ++i)
    {
        given[i] = std::round(delta * weights[i] / total);
        assigned += given[i];
    }
    given[heaviest] += delta - assigned;
    // An own side holds every split nearer to the viewport, and its size is what its parent has, so it
    // changes by what its own other side and every farther one give.
    float farther = 0.0f;
    for (size_t i = splits.size(); i-- > 0;)
    {
        farther += given[i];
        splits[i].own->Size[axis] += farther;
        splits[i].own->WantLockSizeOnce = true;
    }
}

ImGuiID HashVisibleDockNodes(const ImGuiDockNode* node, ImGuiID seed)
{
    if (node == nullptr || !node->IsVisible)
    {
        return seed;
    }
    seed = ImHashData(&node->ID, sizeof(node->ID), seed);
    seed = HashVisibleDockNodes(node->ChildNodes[0], seed);
    return HashVisibleDockNodes(node->ChildNodes[1], seed);
}

// Fits the splits round the viewport's dock node to the picture's size (see ViewportAutoLayout), when
// what the fit depends on changed: the picture's size, the dock space's, the space round the picture
// and the dock nodes that show.
void FitViewportDock(ImGuiID dockspaceId, const ViewportAutoLayout& layout, ImGuiID& fittedKey)
{
    ImGuiWindow* window = layout.windowName != nullptr ? ImGui::FindWindowByName(layout.windowName) : nullptr;
    ImGuiDockNode* leaf = window != nullptr && window->DockIsActive ? window->DockNode : nullptr;
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId);
    if (leaf == nullptr || root == nullptr || ImGui::DockNodeGetRootNode(leaf) != root || !leaf->IsVisible ||
        layout.panelArea.x < 1.0f || layout.panelArea.y < 1.0f || layout.imageSize.x < 1.0f || layout.imageSize.y < 1.0f)
    {
        return;
    }

    // The tab bar, and anything else the panel's window has round its picture.
    const ImVec2 chrome(std::round(leaf->Size.x - layout.panelArea.x), std::round(leaf->Size.y - layout.panelArea.y));
    const float key[] = {layout.imageSize.x, layout.imageSize.y, root->Size.x, root->Size.y, chrome.x, chrome.y, layout.uiScale};
    const ImGuiID frameKey = HashVisibleDockNodes(root, ImHashData(key, sizeof(key), leaf->ID));
    if (frameKey == fittedKey)
    {
        return;
    }
    fittedKey = frameKey;

    const float spacing = ImGui::GetStyle().DockingSeparatorSize;
    const float minPanel = std::round(kAutoLayoutMinPanelPoints * layout.uiScale);
    const std::vector<ViewportSplit> splits[2] = {
        CollectViewportSplits(leaf, ImGuiAxis_X, minPanel, spacing), CollectViewportSplits(leaf, ImGuiAxis_Y, minPanel, spacing)};
    // The most the picture can have: its space now and all that the panels round it can give.
    ImVec2 room = layout.panelArea;
    for (int axis = 0; axis < 2; ++axis)
    {
        for (const ViewportSplit& split : splits[axis])
        {
            room[axis] += split.slack;
        }
    }
    // One display pixel per output pixel when it fits, else the largest it can be at its aspect.
    ImVec2 picture = layout.imageSize;
    const float aspect = layout.imageSize.x / layout.imageSize.y;
    if (picture.x > room.x || picture.y > room.y)
    {
        if (room.x / room.y > aspect)
        {
            picture.y = std::floor(room.y);
            picture.x = std::max(std::round(picture.y * aspect), 1.0f);
        }
        else
        {
            picture.x = std::floor(room.x);
            picture.y = std::max(std::round(picture.x / aspect), 1.0f);
        }
    }
    ResizeAlongAxis(leaf, splits[ImGuiAxis_X], ImGuiAxis_X, picture.x + chrome.x);
    ResizeAlongAxis(leaf, splits[ImGuiAxis_Y], ImGuiAxis_Y, picture.y + chrome.y);
}
}

ImGuiID DrawEditorDockspace(
    bool resetLayout, std::span<EditorPanel* const> panels, const std::optional<ViewportAutoLayout>& autoLayout, ImGuiID& fittedKey)
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
    else if (autoLayout.has_value())
    {
        // Before the dock space lays its nodes out this frame, so the panels draw at the fitted sizes.
        FitViewportDock(dockspaceId, *autoLayout, fittedKey);
    }
    if (!autoLayout.has_value())
    {
        fittedKey = 0;
    }
    ImGui::DockSpaceOverViewport(dockspaceId, mainViewport, kEditorDockspaceFlags);
    EnsureDefaultDockLayout(dockspaceId, mainViewport->WorkSize, panels);
    return dockspaceId;
}
}
