#include "editor_material_graph.h"
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
constexpr float kMaterialGraphResizeBorderPixels = 10.0f;

std::string ImportTextureIntoModelMaterialDirectory(
    const std::string& modelPath,
    uint32_t materialIndex,
    const char* slotName,
    const std::string& sourceTexturePath);

bool DrawGraphTextureSlotEditor(
    const char* label,
    const char* idSuffix,
    const std::string& modelPath,
    uint32_t materialIndex,
    const char* slotName,
    std::string& path,
    std::string* statusMessage = nullptr)
{
    bool changed = false;
    const std::string compactPathLabel = [&path]()
    {
        if (path.empty())
        {
            return std::string("<default input>");
        }

        std::string fileName = std::filesystem::path(path).filename().string();
        if (fileName.empty())
        {
            fileName = path;
        }
        if (fileName.size() <= 26)
        {
            return fileName;
        }

        return fileName.substr(0, 23) + "...";
    }();

    ImGui::PushID(idSuffix);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.0f, 10.0f);
    if (path.empty())
    {
        ImGui::TextDisabled("Default");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.82f, 0.88f, 0.96f, 1.0f), "%s", compactPathLabel.c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", path.c_str());
        }
    }

    if (ImGui::GetContentRegionAvail().x > 120.0f)
    {
        ImGui::SameLine();
    }
    if (ImGui::SmallButton(path.empty() ? "Pick" : "Swap"))
    {
        if (const std::optional<std::string> selectedPath = OpenTextureFileDialog(); selectedPath.has_value())
        {
            try
            {
                path =
                    !modelPath.empty() && slotName != nullptr
                        ? ImportTextureIntoModelMaterialDirectory(modelPath, materialIndex, slotName, *selectedPath)
                        : *selectedPath;
                if (statusMessage != nullptr)
                {
                    *statusMessage = "Imported texture for " + std::string(label) + ": " + path;
                }
                changed = true;
            }
            catch (const std::exception& error)
            {
                if (statusMessage != nullptr)
                {
                    *statusMessage = error.what();
                }
            }
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(path.empty());
    if (ImGui::SmallButton("Clear"))
    {
        path.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}
}

const char* GetMaterialGraphNodeTypeLabel(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return "Texture";
    case MaterialShaderNodeType::Scalar:
        return "Scalar";
    case MaterialShaderNodeType::Color:
        return "Color";
    case MaterialShaderNodeType::Surface:
        return "Surface";
    case MaterialShaderNodeType::Blend:
        return "Blend";
    case MaterialShaderNodeType::Output:
        return "Output";
    default:
        return "Node";
    }
}

const char* GetDefaultMaterialGraphNodeName(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return "Texture";
    case MaterialShaderNodeType::Scalar:
        return "Scalar";
    case MaterialShaderNodeType::Color:
        return "Color";
    case MaterialShaderNodeType::Surface:
        return "Surface";
    case MaterialShaderNodeType::Blend:
        return "Blend";
    case MaterialShaderNodeType::Output:
        return "Material Output";
    default:
        return "Node";
    }
}

ImVec4 GetMaterialGraphHeaderColor(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return ImVec4(0.22f, 0.38f, 0.64f, 1.0f);
    case MaterialShaderNodeType::Scalar:
        return ImVec4(0.48f, 0.33f, 0.17f, 1.0f);
    case MaterialShaderNodeType::Color:
        return ImVec4(0.48f, 0.20f, 0.26f, 1.0f);
    case MaterialShaderNodeType::Surface:
        return ImVec4(0.19f, 0.45f, 0.34f, 1.0f);
    case MaterialShaderNodeType::Blend:
        return ImVec4(0.47f, 0.36f, 0.17f, 1.0f);
    case MaterialShaderNodeType::Output:
        return ImVec4(0.31f, 0.28f, 0.56f, 1.0f);
    default:
        return ImVec4(0.20f, 0.24f, 0.30f, 1.0f);
    }
}

ImU32 GetMaterialGraphPinColor(MaterialGraphPinKind kind)
{
    switch (kind)
    {
    case MaterialGraphPinKind::Texture:
        return IM_COL32(108, 182, 255, 255);
    case MaterialGraphPinKind::Scalar:
        return IM_COL32(255, 188, 102, 255);
    case MaterialGraphPinKind::Color:
        return IM_COL32(244, 132, 132, 255);
    case MaterialGraphPinKind::Surface:
        return IM_COL32(170, 220, 168, 255);
    default:
        return IM_COL32(196, 204, 218, 255);
    }
}

const std::vector<MaterialGraphPinDefinition>& GetMaterialGraphInputPins(MaterialShaderNodeType type)
{
    static const std::vector<MaterialGraphPinDefinition> kEmptyPins{};
    static const std::vector<MaterialGraphPinDefinition> kSurfacePins = {
        {"base_color", "Base Color", MaterialGraphPinKind::Texture},
        {"normal", "Normal", MaterialGraphPinKind::Texture},
        {"metallic", "Metallic", MaterialGraphPinKind::Texture},
        {"roughness", "Roughness", MaterialGraphPinKind::Texture},
        {"occlusion", "Occlusion", MaterialGraphPinKind::Texture},
        {"emissive", "Emissive", MaterialGraphPinKind::Texture}};
    static const std::vector<MaterialGraphPinDefinition> kBlendPins = {
        {"surface_a", "Surface A", MaterialGraphPinKind::Surface},
        {"surface_b", "Surface B", MaterialGraphPinKind::Surface},
        {"mask", "Mask", MaterialGraphPinKind::Texture},
        {"factor", "Factor", MaterialGraphPinKind::Scalar}};
    static const std::vector<MaterialGraphPinDefinition> kOutputPins = {
        {"surface", "Surface", MaterialGraphPinKind::Surface},
        {"base_factor", "Base Factor", MaterialGraphPinKind::Color},
        {"metallic_factor", "Metallic", MaterialGraphPinKind::Scalar},
        {"roughness_factor", "Roughness", MaterialGraphPinKind::Scalar},
        {"normal_scale", "Normal Scale", MaterialGraphPinKind::Scalar},
        {"ao_strength", "AO Strength", MaterialGraphPinKind::Scalar},
        {"emissive_color", "Emissive", MaterialGraphPinKind::Color},
        {"emissive_intensity", "Emissive Intensity", MaterialGraphPinKind::Scalar},
        {"opacity", "Opacity", MaterialGraphPinKind::Scalar}};

    switch (type)
    {
    case MaterialShaderNodeType::Surface:
        return kSurfacePins;
    case MaterialShaderNodeType::Blend:
        return kBlendPins;
    case MaterialShaderNodeType::Output:
        return kOutputPins;
    default:
        return kEmptyPins;
    }
}

const std::vector<MaterialGraphPinDefinition>& GetMaterialGraphOutputPins(MaterialShaderNodeType type)
{
    static const std::vector<MaterialGraphPinDefinition> kEmptyPins{};
    static const std::vector<MaterialGraphPinDefinition> kTexturePins = {
        {"texture", "Texture", MaterialGraphPinKind::Texture}};
    static const std::vector<MaterialGraphPinDefinition> kScalarPins = {
        {"value", "Value", MaterialGraphPinKind::Scalar}};
    static const std::vector<MaterialGraphPinDefinition> kColorPins = {
        {"color", "Color", MaterialGraphPinKind::Color}};
    static const std::vector<MaterialGraphPinDefinition> kSurfacePins = {
        {"surface", "Surface", MaterialGraphPinKind::Surface}};

    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return kTexturePins;
    case MaterialShaderNodeType::Scalar:
        return kScalarPins;
    case MaterialShaderNodeType::Color:
        return kColorPins;
    case MaterialShaderNodeType::Surface:
    case MaterialShaderNodeType::Blend:
        return kSurfacePins;
    default:
        return kEmptyPins;
    }
}

const MaterialGraphPinDefinition* FindMaterialGraphPinDefinition(
    const std::vector<MaterialGraphPinDefinition>& pins,
    std::string_view slot)
{
    const auto iterator = std::find_if(pins.begin(), pins.end(), [slot](const MaterialGraphPinDefinition& pin)
                                       {
                                           return pin.slot == slot;
                                       });
    return iterator != pins.end() ? &(*iterator) : nullptr;
}

const MaterialShaderNode* FindMaterialGraphNode(const MaterialShaderGraph& graph, uint32_t nodeId)
{
    const auto iterator = std::find_if(graph.nodes.begin(), graph.nodes.end(), [nodeId](const MaterialShaderNode& node)
                                       {
                                           return node.id == nodeId;
                                       });
    return iterator != graph.nodes.end() ? &(*iterator) : nullptr;
}

MaterialShaderNode* FindMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId)
{
    const auto iterator = std::find_if(graph.nodes.begin(), graph.nodes.end(), [nodeId](const MaterialShaderNode& node)
                                       {
                                           return node.id == nodeId;
                                       });
    return iterator != graph.nodes.end() ? &(*iterator) : nullptr;
}

const MaterialShaderLink* FindMaterialGraphLink(const MaterialShaderGraph& graph, uint32_t linkId)
{
    const auto iterator = std::find_if(graph.links.begin(), graph.links.end(), [linkId](const MaterialShaderLink& link)
                                       {
                                           return link.id == linkId;
                                       });
    return iterator != graph.links.end() ? &(*iterator) : nullptr;
}

const MaterialShaderLink* FindIncomingMaterialGraphLink(
    const MaterialShaderGraph& graph,
    uint32_t nodeId,
    std::string_view slot)
{
    const auto iterator = std::find_if(graph.links.begin(), graph.links.end(), [&](const MaterialShaderLink& link)
                                       {
                                           return link.toNodeId == nodeId && link.toSlot == slot;
                                       });
    return iterator != graph.links.end() ? &(*iterator) : nullptr;
}

const MaterialGraphRenderedPin* FindRenderedMaterialGraphPin(
    const std::vector<MaterialGraphRenderedPin>& pins,
    uint32_t nodeId,
    std::string_view slot,
    bool input)
{
    const auto iterator = std::find_if(pins.begin(), pins.end(), [&](const MaterialGraphRenderedPin& pin)
                                       {
                                           return pin.nodeId == nodeId && pin.input == input && pin.slot == slot;
                                       });
    return iterator != pins.end() ? &(*iterator) : nullptr;
}

bool MaterialGraphHasOutputNode(const MaterialShaderGraph& graph)
{
    return std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const MaterialShaderNode& node)
                       {
                           return node.type == MaterialShaderNodeType::Output;
                       });
}

std::string BuildMaterialGraphNodeName(MaterialShaderNodeType type, uint32_t nodeId)
{
    if (type == MaterialShaderNodeType::Output)
    {
        return "Material Output";
    }
    return std::string(GetDefaultMaterialGraphNodeName(type)) + " " + std::to_string(nodeId);
}

MaterialShaderNode* AddMaterialGraphNode(
    MaterialShaderGraph& graph,
    MaterialShaderNodeType type,
    const MaterialGraphNodePosition& position)
{
    MaterialShaderNode node{};
    node.id = graph.nextNodeId++;
    node.type = type;
    node.name = BuildMaterialGraphNodeName(type, node.id);
    node.position = position;
    if (type == MaterialShaderNodeType::Blend)
    {
        node.scalarValue = 0.5f;
    }
    graph.nodes.push_back(node);
    return &graph.nodes.back();
}

void RemoveMaterialGraphLink(MaterialShaderGraph& graph, uint32_t linkId)
{
    graph.links.erase(
        std::remove_if(
            graph.links.begin(),
            graph.links.end(),
            [linkId](const MaterialShaderLink& link)
            {
                return link.id == linkId;
            }),
        graph.links.end());
}

void RemoveMaterialGraphIncomingLink(
    MaterialShaderGraph& graph,
    uint32_t nodeId,
    std::string_view slot)
{
    graph.links.erase(
        std::remove_if(
            graph.links.begin(),
            graph.links.end(),
            [nodeId, slot](const MaterialShaderLink& link)
            {
                return link.toNodeId == nodeId && link.toSlot == slot;
            }),
        graph.links.end());
}

void RemoveMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId)
{
    graph.links.erase(
        std::remove_if(
            graph.links.begin(),
            graph.links.end(),
            [nodeId](const MaterialShaderLink& link)
            {
                return link.fromNodeId == nodeId || link.toNodeId == nodeId;
            }),
        graph.links.end());
    graph.nodes.erase(
        std::remove_if(
            graph.nodes.begin(),
            graph.nodes.end(),
            [nodeId](const MaterialShaderNode& node)
            {
                return node.id == nodeId;
            }),
        graph.nodes.end());
}

bool WouldMaterialGraphCreateCycle(
    const MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    uint32_t toNodeId)
{
    std::vector<uint32_t> pending{toNodeId};
    std::vector<uint32_t> visited;

    while (!pending.empty())
    {
        const uint32_t currentNodeId = pending.back();
        pending.pop_back();

        if (currentNodeId == fromNodeId)
        {
            return true;
        }

        if (std::find(visited.begin(), visited.end(), currentNodeId) != visited.end())
        {
            continue;
        }
        visited.push_back(currentNodeId);

        for (const MaterialShaderLink& link : graph.links)
        {
            if (link.fromNodeId == currentNodeId)
            {
                pending.push_back(link.toNodeId);
            }
        }
    }

    return false;
}

bool CanConnectMaterialGraphPins(
    const MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    std::string_view fromSlot,
    uint32_t toNodeId,
    std::string_view toSlot,
    std::string* failureReason = nullptr)
{
    const MaterialShaderNode* fromNode = FindMaterialGraphNode(graph, fromNodeId);
    const MaterialShaderNode* toNode = FindMaterialGraphNode(graph, toNodeId);
    if (fromNode == nullptr || toNode == nullptr)
    {
        if (failureReason != nullptr)
        {
            *failureReason = "The selected node pin is no longer available.";
        }
        return false;
    }

    if (fromNodeId == toNodeId)
    {
        if (failureReason != nullptr)
        {
            *failureReason = "A node cannot be connected to itself.";
        }
        return false;
    }

    const MaterialGraphPinDefinition* outputPin =
        FindMaterialGraphPinDefinition(GetMaterialGraphOutputPins(fromNode->type), fromSlot);
    const MaterialGraphPinDefinition* inputPin =
        FindMaterialGraphPinDefinition(GetMaterialGraphInputPins(toNode->type), toSlot);
    if (outputPin == nullptr || inputPin == nullptr)
    {
        if (failureReason != nullptr)
        {
            *failureReason = "That connection uses an invalid pin.";
        }
        return false;
    }

    if (outputPin->kind != inputPin->kind)
    {
        if (failureReason != nullptr)
        {
            *failureReason = "Only pins with the same data type can be connected.";
        }
        return false;
    }

    if (WouldMaterialGraphCreateCycle(graph, fromNodeId, toNodeId))
    {
        if (failureReason != nullptr)
        {
            *failureReason = "That connection would create a cycle in the material graph.";
        }
        return false;
    }

    return true;
}

uint32_t ConnectMaterialGraphPins(
    MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    std::string_view fromSlot,
    uint32_t toNodeId,
    std::string_view toSlot)
{
    if (!CanConnectMaterialGraphPins(graph, fromNodeId, fromSlot, toNodeId, toSlot))
    {
        return 0;
    }

    for (const MaterialShaderLink& existingLink : graph.links)
    {
        if (existingLink.fromNodeId == fromNodeId &&
            existingLink.fromSlot == fromSlot &&
            existingLink.toNodeId == toNodeId &&
            existingLink.toSlot == toSlot)
        {
            return existingLink.id;
        }
    }

    RemoveMaterialGraphIncomingLink(graph, toNodeId, toSlot);

    MaterialShaderLink link{};
    link.id = graph.nextLinkId++;
    link.fromNodeId = fromNodeId;
    link.fromSlot = std::string(fromSlot);
    link.toNodeId = toNodeId;
    link.toSlot = std::string(toSlot);
    graph.links.push_back(link);
    return link.id;
}

ImVec2 ComputeNodeScreenPosition(
    const MaterialGraphNodePosition& position,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float zoom)
{
    return ImVec2(
        canvasOrigin.x + (position.x - viewOrigin.x) * zoom,
        canvasOrigin.y + (position.y - viewOrigin.y) * zoom);
}

MaterialGraphNodePosition ComputeMaterialGraphPositionFromScreen(
    const ImVec2& screenPosition,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float zoom)
{
    return MaterialGraphNodePosition{
        viewOrigin.x + (screenPosition.x - canvasOrigin.x) / zoom,
        viewOrigin.y + (screenPosition.y - canvasOrigin.y) / zoom};
}

ImVec2 GetMaterialGraphNodeBaseSize(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return ImVec2(320.0f, 140.0f);
    case MaterialShaderNodeType::Scalar:
        return ImVec2(240.0f, 132.0f);
    case MaterialShaderNodeType::Color:
        return ImVec2(260.0f, 152.0f);
    case MaterialShaderNodeType::Surface:
        return ImVec2(280.0f, 310.0f);
    case MaterialShaderNodeType::Blend:
        return ImVec2(280.0f, 220.0f);
    case MaterialShaderNodeType::Output:
        return ImVec2(360.0f, 410.0f);
    default:
        return ImVec2(280.0f, 180.0f);
    }
}

ImVec2 GetMaterialGraphNodeMinSize(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return ImVec2(220.0f, 128.0f);
    case MaterialShaderNodeType::Scalar:
        return ImVec2(180.0f, 124.0f);
    case MaterialShaderNodeType::Color:
        return ImVec2(200.0f, 140.0f);
    case MaterialShaderNodeType::Surface:
        return ImVec2(240.0f, 260.0f);
    case MaterialShaderNodeType::Blend:
        return ImVec2(220.0f, 188.0f);
    case MaterialShaderNodeType::Output:
        return ImVec2(300.0f, 340.0f);
    default:
        return ImVec2(200.0f, 140.0f);
    }
}

ImVec2 GetMaterialGraphNodeLogicalSize(const MaterialShaderNode& node)
{
    const ImVec2 baseSize = GetMaterialGraphNodeBaseSize(node.type);
    return ImVec2(
        node.width > 0.0f ? node.width : baseSize.x,
        node.height > 0.0f ? node.height : baseSize.y);
}

void SetMaterialGraphNodeLogicalSize(MaterialShaderNode& node, const ImVec2& logicalSize)
{
    const ImVec2 baseSize = GetMaterialGraphNodeBaseSize(node.type);
    node.width = std::abs(logicalSize.x - baseSize.x) <= 0.01f ? 0.0f : logicalSize.x;
    node.height = std::abs(logicalSize.y - baseSize.y) <= 0.01f ? 0.0f : logicalSize.y;
}

ImVec2 GetMaterialGraphNodeSize(const MaterialShaderNode& node, float uiScale)
{
    const ImVec2 logicalSize = GetMaterialGraphNodeLogicalSize(node);
    return ImVec2(logicalSize.x * uiScale, logicalSize.y * uiScale);
}

uint8_t GetMaterialGraphResizeEdges(const ImRect& nodeRect, const ImVec2& mousePosition, float uiScale)
{
    if (!nodeRect.Contains(mousePosition))
    {
        return MaterialGraphResizeEdge_None;
    }

    const float borderThickness = kMaterialGraphResizeBorderPixels * uiScale;
    const ImRect innerRect(
        ImVec2(nodeRect.Min.x + borderThickness, nodeRect.Min.y + borderThickness),
        ImVec2(nodeRect.Max.x - borderThickness, nodeRect.Max.y - borderThickness));
    if (innerRect.Contains(mousePosition))
    {
        return MaterialGraphResizeEdge_None;
    }

    uint8_t edges = MaterialGraphResizeEdge_None;
    if (mousePosition.x <= nodeRect.Min.x + borderThickness)
    {
        edges |= MaterialGraphResizeEdge_Left;
    }
    else if (mousePosition.x >= nodeRect.Max.x - borderThickness)
    {
        edges |= MaterialGraphResizeEdge_Right;
    }

    if (mousePosition.y <= nodeRect.Min.y + borderThickness)
    {
        edges |= MaterialGraphResizeEdge_Top;
    }
    else if (mousePosition.y >= nodeRect.Max.y - borderThickness)
    {
        edges |= MaterialGraphResizeEdge_Bottom;
    }

    return edges;
}

ImGuiMouseCursor GetMaterialGraphResizeCursor(uint8_t edges)
{
    const bool horizontal = (edges & (MaterialGraphResizeEdge_Left | MaterialGraphResizeEdge_Right)) != 0;
    const bool vertical = (edges & (MaterialGraphResizeEdge_Top | MaterialGraphResizeEdge_Bottom)) != 0;
    if (horizontal && vertical)
    {
        const bool northwestToSoutheast =
            ((edges & MaterialGraphResizeEdge_Left) != 0 && (edges & MaterialGraphResizeEdge_Top) != 0) ||
            ((edges & MaterialGraphResizeEdge_Right) != 0 && (edges & MaterialGraphResizeEdge_Bottom) != 0);
        return northwestToSoutheast ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW;
    }
    if (horizontal)
    {
        return ImGuiMouseCursor_ResizeEW;
    }
    if (vertical)
    {
        return ImGuiMouseCursor_ResizeNS;
    }
    return ImGuiMouseCursor_Arrow;
}

bool CanPasteMaterialGraphNode(
    const MaterialShaderGraph& graph,
    const std::optional<MaterialShaderNode>& clipboardNode)
{
    if (!clipboardNode.has_value())
    {
        return false;
    }
    if (clipboardNode->type == MaterialShaderNodeType::Output && MaterialGraphHasOutputNode(graph))
    {
        return false;
    }
    return true;
}

MaterialShaderNode* PasteMaterialGraphNode(
    MaterialShaderGraph& graph,
    const MaterialShaderNode& clipboardNode,
    const MaterialGraphNodePosition& position)
{
    if (clipboardNode.type == MaterialShaderNodeType::Output && MaterialGraphHasOutputNode(graph))
    {
        return nullptr;
    }

    MaterialShaderNode newNode = clipboardNode;
    newNode.id = graph.nextNodeId++;
    newNode.position = position;
    if (!clipboardNode.name.empty())
    {
        newNode.name = clipboardNode.name + " Copy";
    }
    else
    {
        newNode.name = BuildMaterialGraphNodeName(clipboardNode.type, newNode.id);
    }
    graph.nodes.push_back(newNode);
    return &graph.nodes.back();
}

bool ApplyMaterialGraphNodeResize(
    MaterialShaderNode& node,
    uint8_t resizeEdges,
    const MaterialGraphNodePosition& startPosition,
    const ImVec2& startLogicalSize,
    const ImVec2& mouseDelta,
    float effectiveUiScale,
    float zoom)
{
    if (effectiveUiScale <= 0.0f || zoom <= 0.0f || resizeEdges == MaterialGraphResizeEdge_None)
    {
        return false;
    }

    MaterialGraphNodePosition updatedPosition = startPosition;
    ImVec2 updatedSize = startLogicalSize;
    const ImVec2 minSize = GetMaterialGraphNodeMinSize(node.type);

    if ((resizeEdges & MaterialGraphResizeEdge_Left) != 0)
    {
        const float rightGraph = startPosition.x + startLogicalSize.x * effectiveUiScale;
        const float nextLeftGraph = startPosition.x + mouseDelta.x / zoom;
        updatedSize.x = std::max(minSize.x, (rightGraph - nextLeftGraph) / effectiveUiScale);
        updatedPosition.x = rightGraph - updatedSize.x * effectiveUiScale;
    }
    else if ((resizeEdges & MaterialGraphResizeEdge_Right) != 0)
    {
        updatedSize.x = std::max(minSize.x, startLogicalSize.x + mouseDelta.x / (effectiveUiScale * zoom));
    }

    if ((resizeEdges & MaterialGraphResizeEdge_Top) != 0)
    {
        const float bottomGraph = startPosition.y + startLogicalSize.y * effectiveUiScale;
        const float nextTopGraph = startPosition.y + mouseDelta.y / zoom;
        updatedSize.y = std::max(minSize.y, (bottomGraph - nextTopGraph) / effectiveUiScale);
        updatedPosition.y = bottomGraph - updatedSize.y * effectiveUiScale;
    }
    else if ((resizeEdges & MaterialGraphResizeEdge_Bottom) != 0)
    {
        updatedSize.y = std::max(minSize.y, startLogicalSize.y + mouseDelta.y / (effectiveUiScale * zoom));
    }

    const ImVec2 previousSize = GetMaterialGraphNodeLogicalSize(node);
    const bool positionChanged =
        std::abs(node.position.x - updatedPosition.x) > 0.01f || std::abs(node.position.y - updatedPosition.y) > 0.01f;
    const bool sizeChanged =
        std::abs(previousSize.x - updatedSize.x) > 0.01f || std::abs(previousSize.y - updatedSize.y) > 0.01f;
    if (!positionChanged && !sizeChanged)
    {
        return false;
    }

    node.position = updatedPosition;
    SetMaterialGraphNodeLogicalSize(node, updatedSize);
    return true;
}

ImRect BuildMaterialGraphNodeRect(
    const MaterialShaderNode& node,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float uiScale,
    float zoom)
{
    const ImVec2 nodePosition = ComputeNodeScreenPosition(node.position, canvasOrigin, viewOrigin, zoom);
    const ImVec2 nodeSize = GetMaterialGraphNodeSize(node, uiScale);
    return ImRect(nodePosition, ImVec2(nodePosition.x + nodeSize.x, nodePosition.y + nodeSize.y));
}

bool IsMouseOverMaterialGraphNode(
    const MaterialShaderGraph& graph,
    const ImVec2& mousePosition,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float uiScale,
    float zoom)
{
    return std::any_of(graph.nodes.begin(), graph.nodes.end(), [&](const MaterialShaderNode& node)
                       {
                           return BuildMaterialGraphNodeRect(node, canvasOrigin, viewOrigin, uiScale, zoom).Contains(mousePosition);
                       });
}

std::string BuildMaterialGraphTextureImportSlotName(const MaterialShaderNode& node)
{
    return "graph_node_" + std::to_string(node.id) + "_texture";
}

bool DrawMaterialPbrControls(MaterialPbrSurfaceSettings& pbr)
{
    bool changed = false;
    changed |= ImGui::ColorEdit4("Base Factor", pbr.baseColorFactor);
    changed |= ImGui::SliderFloat("Metallic", &pbr.metallicFactor, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Roughness", &pbr.roughnessFactor, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Normal Scale", &pbr.normalScale, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("AO Strength", &pbr.occlusionStrength, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::ColorEdit3("Emissive", pbr.emissiveColor);
    changed |= ImGui::SliderFloat("Emissive Intensity", &pbr.emissiveIntensity, 0.0f, 8.0f, "%.2f");
    changed |= ImGui::SliderFloat("Opacity", &pbr.opacity, 0.0f, 1.0f, "%.2f");
    return changed;
}

MaterialGraphRenderedPin DrawMaterialGraphPinRow(
    const MaterialGraphPinDefinition& pinDefinition,
    uint32_t nodeId,
    bool input,
    float uiScale,
    bool linkDragActive,
    bool highlightCompatible,
    bool selected,
    uint32_t linkId,
    bool& pinPressed,
    bool& disconnectRequested)
{
    const float pinRadius = 6.0f * uiScale;
    const float pinDiameter = pinRadius * 2.0f + 6.0f * uiScale;
    const ImU32 pinColor = GetMaterialGraphPinColor(pinDefinition.kind);

    MaterialGraphRenderedPin renderedPin{};
    renderedPin.nodeId = nodeId;
    renderedPin.slot = pinDefinition.slot;
    renderedPin.kind = pinDefinition.kind;
    renderedPin.input = input;
    renderedPin.linkId = linkId;

    ImGui::PushID(pinDefinition.slot);
    if (!input)
    {
        const float labelWidth = ImGui::CalcTextSize(pinDefinition.label).x;
        const float rowWidth = labelWidth + pinDiameter + 10.0f * uiScale;
        const float outputStartX = std::max(
            ImGui::GetCursorPosX(),
            ImGui::GetWindowContentRegionMax().x - rowWidth);
        ImGui::SetCursorPosX(outputStartX);
        ImGui::TextUnformatted(pinDefinition.label);
        ImGui::SameLine(0.0f, 8.0f * uiScale);
    }

    ImGui::InvisibleButton(input ? "InputPin" : "OutputPin", ImVec2(pinDiameter, pinDiameter));
    const bool hovered = ImGui::IsItemHovered();
    pinPressed = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    renderedPin.center = ImVec2(
        (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
        (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);

    ImU32 fillColor = pinColor;
    if (linkDragActive && highlightCompatible)
    {
        fillColor = IM_COL32(255, 214, 122, 255);
    }
    else if (selected)
    {
        fillColor = IM_COL32(255, 196, 64, 255);
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddCircleFilled(renderedPin.center, pinRadius, fillColor, 14);
    drawList->AddCircle(
        renderedPin.center,
        pinRadius + 1.25f * uiScale,
        hovered ? IM_COL32(255, 255, 255, 240) : IM_COL32(24, 28, 34, 230),
        14,
        1.5f * uiScale);

    if (input)
    {
        ImGui::SameLine(0.0f, 8.0f * uiScale);
        ImGui::TextUnformatted(pinDefinition.label);
        if (linkId != 0)
        {
            ImGui::SameLine(0.0f, 8.0f * uiScale);
            ImGui::TextDisabled("linked");
            ImGui::SameLine(0.0f, 6.0f * uiScale);
            disconnectRequested = ImGui::SmallButton("X");
        }
    }

    ImGui::PopID();
    return renderedPin;
}

MaterialGraphNodeDrawResult DrawMaterialGraphNode(
    ModelImportedMaterialInfo& material,
    MaterialShaderNode& node,
    const std::string& modelPath,
    uint32_t materialIndex,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float uiScale,
    float zoom,
    bool nodeSelected,
    bool linkDragActive,
    bool nodeResizeActive,
    bool canPasteClipboardNode,
    uint32_t dragFromNodeId,
    std::string_view dragFromSlot,
    std::string* statusMessage)
{
    MaterialGraphNodeDrawResult result{};
    const ImVec2 nodeSize = GetMaterialGraphNodeSize(node, uiScale);
    const ImVec4 headerColor = GetMaterialGraphHeaderColor(node.type);
    const bool allowDelete = node.type != MaterialShaderNodeType::Output;
    const float cornerRounding = 10.0f * uiScale;
    const float headerHeight = 34.0f * uiScale;
    const ImU32 headerFillColor = ImGui::ColorConvertFloat4ToU32(headerColor);
    const ImU32 nodeFillColor = nodeSelected ? IM_COL32(25, 30, 38, 248) : IM_COL32(18, 22, 29, 244);
    const ImU32 nodeBorderColor = nodeSelected ? kSelectionOutlineColor : IM_COL32(76, 90, 108, 224);
    const ImU32 titleColor = nodeSelected ? IM_COL32(252, 246, 228, 255) : IM_COL32(235, 241, 248, 255);
    const ImU32 badgeColor = nodeSelected ? IM_COL32(255, 236, 190, 235) : IM_COL32(242, 246, 252, 220);
    const ImVec2 nodePosition = ComputeNodeScreenPosition(node.position, canvasOrigin, viewOrigin, zoom);

    ImGui::SetCursorScreenPos(nodePosition);
    ImGui::PushID(static_cast<int>(node.id));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * uiScale, 10.0f * uiScale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("Node", nodeSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowFontScale(zoom);

    ImGuiWindow* nodeWindow = ImGui::GetCurrentWindow();
    const ImVec2 nodeMin = ImGui::GetWindowPos();
    const ImVec2 nodeMax(nodeMin.x + nodeSize.x, nodeMin.y + nodeSize.y);
    const ImRect nodeRect(nodeMin, nodeMax);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(nodeMin, nodeMax, nodeFillColor, cornerRounding);
    drawList->AddRectFilled(
        nodeMin,
        ImVec2(nodeMax.x, nodeMin.y + headerHeight),
        headerFillColor,
        cornerRounding);
    drawList->AddRect(
        nodeMin,
        nodeMax,
        nodeBorderColor,
        cornerRounding,
        0,
        nodeSelected ? 2.8f * uiScale : 1.4f * uiScale);

    const char* nodeTitle = node.name.empty() ? GetDefaultMaterialGraphNodeName(node.type) : node.name.c_str();
    const char* nodeTypeLabel = GetMaterialGraphNodeTypeLabel(node.type);
    const ImVec2 badgeMin(nodeMin.x + 10.0f * uiScale, nodeMin.y + 8.0f * uiScale);
    const ImVec2 badgeTextSize = ImGui::CalcTextSize(nodeTypeLabel);
    const ImVec2 badgeMax(
        badgeMin.x + badgeTextSize.x + 14.0f * uiScale,
        badgeMin.y + std::max(18.0f * uiScale, badgeTextSize.y + 6.0f * uiScale));
    drawList->AddRectFilled(badgeMin, badgeMax, badgeColor, 7.0f * uiScale);
    drawList->AddText(
        ImVec2(badgeMin.x + 7.0f * uiScale, badgeMin.y + 3.0f * uiScale),
        IM_COL32(36, 40, 48, 255),
        nodeTypeLabel);
    drawList->AddText(
        ImVec2(badgeMax.x + 10.0f * uiScale, nodeMin.y + 9.0f * uiScale),
        titleColor,
        nodeTitle);

    if (allowDelete)
    {
        ImGui::SetCursorPos(ImVec2(nodeSize.x - 34.0f * uiScale, 7.0f * uiScale));
        if (ImGui::Button("X", ImVec2(24.0f * uiScale, 20.0f * uiScale)))
        {
            result.requestDelete = true;
        }
    }

    ImGui::SetCursorPos(ImVec2(12.0f * uiScale, headerHeight + 10.0f * uiScale));
    ImGui::TextDisabled("%s node", GetMaterialGraphNodeTypeLabel(node.type));

    const auto& inputPins = GetMaterialGraphInputPins(node.type);
    if (!inputPins.empty())
    {
        ImGui::SeparatorText("Inputs");
        for (const MaterialGraphPinDefinition& pinDefinition : inputPins)
        {
            const MaterialShaderLink* incomingLink =
                FindIncomingMaterialGraphLink(material.shaderGraph, node.id, pinDefinition.slot);
            const bool highlightCompatible =
                linkDragActive &&
                CanConnectMaterialGraphPins(
                    material.shaderGraph,
                    dragFromNodeId,
                    dragFromSlot,
                    node.id,
                    pinDefinition.slot);

            bool pinPressed = false;
            bool disconnectRequested = false;
            MaterialGraphRenderedPin renderedPin = DrawMaterialGraphPinRow(
                pinDefinition,
                node.id,
                true,
                uiScale,
                linkDragActive,
                highlightCompatible,
                incomingLink != nullptr,
                incomingLink != nullptr ? incomingLink->id : 0,
                pinPressed,
                disconnectRequested);
            if (pinPressed)
            {
                result.selected = true;
                if (linkDragActive)
                {
                    std::string failureReason;
                    if (CanConnectMaterialGraphPins(
                            material.shaderGraph,
                            dragFromNodeId,
                            dragFromSlot,
                            node.id,
                            pinDefinition.slot,
                            &failureReason))
                    {
                        result.connectedLinkId = ConnectMaterialGraphPins(
                            material.shaderGraph,
                            dragFromNodeId,
                            dragFromSlot,
                            node.id,
                            pinDefinition.slot);
                        result.changed |= result.connectedLinkId != 0;
                    }
                    else if (statusMessage != nullptr && !failureReason.empty())
                    {
                        *statusMessage = failureReason;
                    }
                }
                else if (incomingLink != nullptr)
                {
                    result.selectedLinkId = incomingLink->id;
                }
            }
            if (disconnectRequested && incomingLink != nullptr)
            {
                RemoveMaterialGraphLink(material.shaderGraph, incomingLink->id);
                result.changed = true;
            }
            result.pins.push_back(std::move(renderedPin));
        }
    }

    switch (node.type)
    {
    case MaterialShaderNodeType::Texture:
    {
        ImGui::SeparatorText("Texture");
        ImGui::TextWrapped("This node outputs a texture sample for surface inputs or blend mask wiring.");
        const std::string idSuffix = "TextureNode_" + std::to_string(node.id);
        const std::string slotName = BuildMaterialGraphTextureImportSlotName(node);
        result.changed |= DrawGraphTextureSlotEditor(
            "Source",
            idSuffix.c_str(),
            modelPath,
            materialIndex,
            slotName.c_str(),
            node.texturePath,
            statusMessage);
        break;
    }
    case MaterialShaderNodeType::Scalar:
        ImGui::SeparatorText("Value");
        ImGui::TextWrapped("Use this node to drive blend factor or output scalar overrides.");
        result.changed |= ImGui::SliderFloat("Scalar", &node.scalarValue, 0.0f, 8.0f, "%.2f");
        break;
    case MaterialShaderNodeType::Color:
        ImGui::SeparatorText("Color");
        ImGui::TextWrapped("Feed this into base factor or emissive color inputs.");
        result.changed |= ImGui::ColorEdit4("Color", node.colorValue);
        break;
    case MaterialShaderNodeType::Surface:
        ImGui::SeparatorText("Surface");
        ImGui::TextWrapped("Connect texture nodes into the PBR texture slots, then route the surface output forward.");
        break;
    case MaterialShaderNodeType::Blend:
        ImGui::SeparatorText("Blend");
        ImGui::TextWrapped("This node mixes Surface A and Surface B. A connected scalar input overrides the default factor.");
        result.changed |= ImGui::SliderFloat("Default Factor", &node.scalarValue, 0.0f, 1.0f, "%.2f");
        break;
    case MaterialShaderNodeType::Output:
        ImGui::SeparatorText("PBR Defaults");
        ImGui::TextWrapped("These values are used as material defaults until matching scalar or color inputs are connected.");
        result.changed |= DrawMaterialPbrControls(node.pbr);
        break;
    default:
        break;
    }

    const auto& outputPins = GetMaterialGraphOutputPins(node.type);
    if (!outputPins.empty())
    {
        ImGui::SeparatorText("Outputs");
        for (const MaterialGraphPinDefinition& pinDefinition : outputPins)
        {
            bool pinPressed = false;
            bool disconnectRequested = false;
            MaterialGraphRenderedPin renderedPin = DrawMaterialGraphPinRow(
                pinDefinition,
                node.id,
                false,
                uiScale,
                linkDragActive,
                false,
                linkDragActive &&
                    dragFromNodeId == node.id &&
                    dragFromSlot == pinDefinition.slot,
                0,
                pinPressed,
                disconnectRequested);
            if (pinPressed)
            {
                result.requestStartLinkDrag = true;
                result.startLinkNodeId = node.id;
                result.startLinkSlot = pinDefinition.slot;
                result.selected = true;
            }
            result.pins.push_back(std::move(renderedPin));
        }
    }

    const bool nodeWidgetsHovered = ImGui::IsAnyItemHovered();
    const bool nodeControlsActive = GImGui->ActiveIdWindow == nodeWindow;
    const uint8_t hoveredResizeEdges =
        nodeSelected && !linkDragActive ? GetMaterialGraphResizeEdges(nodeRect, ImGui::GetIO().MousePos, uiScale) : 0;
    result.hovered = nodeRect.Contains(ImGui::GetIO().MousePos);
    result.capturesMouse = result.hovered || nodeControlsActive || nodeResizeActive;
    if (result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        result.selected = true;
    }
    if (nodeSelected &&
        hoveredResizeEdges != MaterialGraphResizeEdge_None &&
        !nodeWidgetsHovered &&
        !nodeControlsActive &&
        !linkDragActive)
    {
        ImGui::SetMouseCursor(GetMaterialGraphResizeCursor(hoveredResizeEdges));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            result.requestStartResize = true;
            result.resizeEdges = hoveredResizeEdges;
            result.selected = true;
        }
    }
    if (nodeSelected &&
        !nodeResizeActive &&
        hoveredResizeEdges == MaterialGraphResizeEdge_None &&
        result.hovered &&
        !nodeControlsActive &&
        !linkDragActive &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        node.position.x += ImGui::GetIO().MouseDelta.x / zoom;
        node.position.y += ImGui::GetIO().MouseDelta.y / zoom;
        result.changed = true;
        result.selected = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    result.min = ImGui::GetItemRectMin();
    result.max = ImGui::GetItemRectMax();

    if (ImGui::BeginPopupContextItem("NodeContextMenu"))
    {
        result.selected = true;
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Copy Node", nullptr, false, allowDelete))
            {
                result.requestCopy = true;
            }
            if (ImGui::MenuItem("Paste Node", nullptr, false, canPasteClipboardNode))
            {
                result.requestPaste = true;
                result.pastePosition = MaterialGraphNodePosition{
                    node.position.x + 40.0f,
                    node.position.y + 40.0f};
            }
            if (ImGui::MenuItem("Delete Node", nullptr, false, allowDelete))
            {
                result.requestDelete = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();
    return result;
}

void DrawNodeConnection(
    ImDrawList* drawList,
    const ImVec2& from,
    const ImVec2& to,
    ImU32 color,
    float thickness)
{
    if (drawList == nullptr)
    {
        return;
    }

    const float tangentOffset = std::max((to.x - from.x) * 0.5f, 70.0f);
    const ImVec2 control0(from.x + tangentOffset, from.y);
    const ImVec2 control1(to.x - tangentOffset, to.y);
    drawList->AddBezierCubic(from, control0, control1, to, color, thickness);
}

namespace
{
std::string ImportTextureIntoModelMaterialDirectory(
    const std::string& modelPath,
    uint32_t materialIndex,
    const char* slotName,
    const std::string& sourceTexturePath)
{
    static_cast<void>(modelPath);
    static_cast<void>(materialIndex);
    static_cast<void>(slotName);
    return sourceTexturePath;
}
}
