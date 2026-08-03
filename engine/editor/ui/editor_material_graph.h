#pragma once

// Shader node graph editor primitives shared between the graph canvas
// implementation and the model processor panel.

#include <scene_components.h>

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

inline constexpr float kMaterialGraphMinZoom = 0.55f;
inline constexpr float kMaterialGraphMaxZoom = 1.8f;
inline constexpr float kMaterialGraphZoomStep = 1.12f;

enum class MaterialGraphPinKind : uint32_t
{
    Texture = 0,
    Scalar = 1,
    Color = 2,
    Surface = 3
};

struct MaterialGraphPinDefinition
{
    const char* slot = "";
    const char* label = "";
    MaterialGraphPinKind kind = MaterialGraphPinKind::Texture;
};

struct MaterialGraphRenderedPin
{
    uint32_t nodeId = 0;
    std::string slot;
    MaterialGraphPinKind kind = MaterialGraphPinKind::Texture;
    bool input = true;
    ImVec2 center{0.0f, 0.0f};
    uint32_t linkId = 0;
};

struct MaterialGraphNodeDrawResult
{
    bool changed = false;
    bool selected = false;
    bool hovered = false;
    bool capturesMouse = false;
    bool requestCopy = false;
    bool requestDelete = false;
    bool requestPaste = false;
    bool requestStartResize = false;
    bool requestStartLinkDrag = false;
    uint32_t startLinkNodeId = 0;
    uint32_t selectedLinkId = 0;
    uint32_t connectedLinkId = 0;
    uint8_t resizeEdges = 0;
    std::string startLinkSlot;
    std::vector<MaterialGraphRenderedPin> pins;
    ImVec2 min{0.0f, 0.0f};
    ImVec2 max{0.0f, 0.0f};
    MaterialGraphNodePosition pastePosition{};
};

enum MaterialGraphResizeEdgeFlags : uint8_t
{
    MaterialGraphResizeEdge_None = 0,
    MaterialGraphResizeEdge_Left = 1 << 0,
    MaterialGraphResizeEdge_Right = 1 << 1,
    MaterialGraphResizeEdge_Top = 1 << 2,
    MaterialGraphResizeEdge_Bottom = 1 << 3
};

const char* GetMaterialGraphNodeTypeLabel(MaterialShaderNodeType type);
const char* GetDefaultMaterialGraphNodeName(MaterialShaderNodeType type);
ImU32 GetMaterialGraphPinColor(MaterialGraphPinKind kind);
const MaterialShaderNode* FindMaterialGraphNode(const MaterialShaderGraph& graph, uint32_t nodeId);
MaterialShaderNode* FindMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId);
const MaterialShaderLink* FindMaterialGraphLink(const MaterialShaderGraph& graph, uint32_t linkId);
const MaterialGraphRenderedPin* FindRenderedMaterialGraphPin(
    const std::vector<MaterialGraphRenderedPin>& pins,
    uint32_t nodeId,
    std::string_view slot,
    bool input);
bool MaterialGraphHasOutputNode(const MaterialShaderGraph& graph);
MaterialShaderNode* AddMaterialGraphNode(
    MaterialShaderGraph& graph,
    MaterialShaderNodeType type,
    const MaterialGraphNodePosition& position);
void RemoveMaterialGraphLink(MaterialShaderGraph& graph, uint32_t linkId);
void RemoveMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId);
MaterialGraphNodePosition ComputeMaterialGraphPositionFromScreen(
    const ImVec2& screenPosition,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float zoom);
ImVec2 GetMaterialGraphNodeLogicalSize(const MaterialShaderNode& node);
ImGuiMouseCursor GetMaterialGraphResizeCursor(uint8_t edges);
bool CanPasteMaterialGraphNode(
    const MaterialShaderGraph& graph,
    const std::optional<MaterialShaderNode>& clipboardNode);
MaterialShaderNode* PasteMaterialGraphNode(
    MaterialShaderGraph& graph,
    const MaterialShaderNode& clipboardNode,
    const MaterialGraphNodePosition& position);
bool ApplyMaterialGraphNodeResize(
    MaterialShaderNode& node,
    uint8_t resizeEdges,
    const MaterialGraphNodePosition& startPosition,
    const ImVec2& startLogicalSize,
    const ImVec2& mouseDelta,
    float effectiveUiScale,
    float zoom);
bool IsMouseOverMaterialGraphNode(
    const MaterialShaderGraph& graph,
    const ImVec2& mousePosition,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float uiScale,
    float zoom);
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
    std::string* statusMessage);
void DrawNodeConnection(
    ImDrawList* drawList,
    const ImVec2& from,
    const ImVec2& to,
    ImU32 color,
    float thickness);
