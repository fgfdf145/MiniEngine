#include "editor_material_graph.h"

#include "editor_menu_toolbar.h"
#include "editor_ui_internal.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <unordered_map>

namespace me
{

namespace
{
const char* const kNoName = "";

// The canvas's colours: Unreal's graph, in the editor's dark theme.
constexpr ImU32 kCanvasBackground = IM_COL32(26, 26, 28, 255);
constexpr ImU32 kGridMinor = IM_COL32(36, 36, 39, 255);
constexpr ImU32 kGridMajor = IM_COL32(16, 16, 18, 255);
constexpr ImU32 kNodeBody = IM_COL32(30, 30, 33, 242);
constexpr ImU32 kNodeBorder = IM_COL32(10, 10, 12, 255);
constexpr ImU32 kNodeHoverBorder = IM_COL32(110, 110, 118, 255);
constexpr ImU32 kNodeSelectedBorder = IM_COL32(255, 160, 40, 255);
constexpr ImU32 kNodeText = IM_COL32(236, 236, 240, 255);
constexpr ImU32 kNodeTextDim = IM_COL32(150, 150, 158, 255);
constexpr ImU32 kBoxFill = IM_COL32(255, 160, 40, 28);
constexpr ImU32 kBoxBorder = IM_COL32(255, 160, 40, 170);

// Layout in graph units at zoom 1 and UI scale 1, times the canvas's scale.
constexpr float kPadding = 8.0f;
constexpr float kRowSpacing = 7.0f;
constexpr float kHeaderSpacing = 11.0f;
constexpr float kPinRadius = 4.5f;
constexpr float kMinNodeWidth = 140.0f;
constexpr float kThumbnailSize = 56.0f;
constexpr float kRounding = 6.0f;

ImVec2 operator+(const ImVec2& a, const ImVec2& b)
{
    return ImVec2(a.x + b.x, a.y + b.y);
}
ImVec2 operator-(const ImVec2& a, const ImVec2& b)
{
    return ImVec2(a.x - b.x, a.y - b.y);
}
ImVec2 operator*(const ImVec2& a, float s)
{
    return ImVec2(a.x * s, a.y * s);
}

float TextWidth(float fontSize, const char* text)
{
    return ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text).x;
}

// The value an input of the Output node shows when nothing is linked into it: the Output's own factor.
std::string OutputFactorText(const MaterialShaderNode& node, std::string_view slot)
{
    const MaterialPbrSurfaceSettings& pbr = node.pbr;
    char text[48] = {};
    if (slot == "metallic_factor")
    {
        std::snprintf(text, sizeof(text), "%.2f", pbr.metallicFactor);
    }
    else if (slot == "roughness_factor")
    {
        std::snprintf(text, sizeof(text), "%.2f", pbr.roughnessFactor);
    }
    else if (slot == "normal_scale")
    {
        std::snprintf(text, sizeof(text), "%.2f", pbr.normalScale);
    }
    else if (slot == "ao_strength")
    {
        std::snprintf(text, sizeof(text), "%.2f", pbr.occlusionStrength);
    }
    else if (slot == "emissive_intensity")
    {
        std::snprintf(text, sizeof(text), "%.3g", pbr.emissiveIntensity);
    }
    else if (slot == "opacity")
    {
        std::snprintf(text, sizeof(text), "%.2f", pbr.opacity);
    }
    return text;
}

// What a pin's row says: its label, or a Scalar's value in place of "Value".
std::string PinText(const MaterialShaderNode& node, const MaterialGraphPinDefinition& pin, bool input)
{
    if (!input && node.type == MaterialShaderNodeType::Scalar)
    {
        char text[32] = {};
        std::snprintf(text, sizeof(text), "%.3g", node.scalarValue);
        return text;
    }
    return pin.label;
}

// The sizes a node's parts come to at a canvas scale (UI scale times zoom) and font size.
struct NodeMetrics
{
    float padding = 0.0f;
    float rowHeight = 0.0f;
    float headerHeight = 0.0f;
    float pinRadius = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    // Texture nodes: the thumbnail's side; Color nodes: the swatch's width.
    float extra = 0.0f;
};

NodeMetrics MeasureNode(const MaterialShaderNode& node, float scale, float fontSize)
{
    NodeMetrics metrics;
    metrics.padding = kPadding * scale;
    metrics.rowHeight = fontSize + kRowSpacing * scale;
    metrics.headerHeight = fontSize + kHeaderSpacing * scale;
    metrics.pinRadius = kPinRadius * scale;
    const auto& inputs = GetMaterialGraphInputPins(node.type);
    const auto& outputs = GetMaterialGraphOutputPins(node.type);
    float inputWidth = 0.0f;
    for (const MaterialGraphPinDefinition& pin : inputs)
    {
        float width = TextWidth(fontSize, pin.label);
        // Room for the factor the Output node shows beside an input nothing is linked into.
        if (node.type == MaterialShaderNodeType::Output && pin.kind == MaterialGraphPinKind::Scalar)
        {
            width += TextWidth(fontSize, "  00.00");
        }
        inputWidth = std::max(inputWidth, width);
    }
    float outputWidth = 0.0f;
    for (const MaterialGraphPinDefinition& pin : outputs)
    {
        outputWidth = std::max(outputWidth, TextWidth(fontSize, PinText(node, pin, false).c_str()));
    }
    const std::string title = MaterialGraphNodeTitle(node);
    const float pinColumn = 2.0f * metrics.pinRadius + 6.0f * scale;
    float width = std::max(
        {kMinNodeWidth * scale,
         TextWidth(fontSize, title.c_str()) + 2.0f * metrics.padding + 10.0f * scale,
         inputWidth + outputWidth + 2.0f * (metrics.padding + pinColumn) + 20.0f * scale});
    const size_t rows = std::max(inputs.size(), outputs.size());
    float body = static_cast<float>(rows) * metrics.rowHeight + metrics.padding;
    if (node.type == MaterialShaderNodeType::Texture)
    {
        metrics.extra = kThumbnailSize * scale;
        width = std::max(width, metrics.extra + outputWidth + 3.0f * metrics.padding + pinColumn);
        // The thumbnail beside the pin, the file's name under both.
        body = std::max(body, metrics.extra + metrics.padding * 1.5f) + fontSize + metrics.padding * 0.5f;
    }
    else if (node.type == MaterialShaderNodeType::Color)
    {
        metrics.extra = 30.0f * scale;
        width = std::max(width, metrics.extra + outputWidth + 3.0f * metrics.padding + pinColumn);
    }
    metrics.width = std::round(width);
    metrics.height = std::round(metrics.headerHeight + body);
    return metrics;
}

float BaseFontSize()
{
    const float size = ImGui::GetStyle().FontSizeBase;
    return size > 0.0f ? size : 13.0f;
}

// Points along the link's curve, from an output pin rightward into an input pin.
void LinkControls(const ImVec2& from, const ImVec2& to, float scale, ImVec2& c0, ImVec2& c1)
{
    const float reach = std::max(std::abs(to.x - from.x) * 0.5f, 50.0f * scale);
    c0 = ImVec2(from.x + reach, from.y);
    c1 = ImVec2(to.x - reach, to.y);
}

float DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
    const ImVec2 ab = b - a;
    const float length2 = ab.x * ab.x + ab.y * ab.y;
    const float t = length2 > 0.0f ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / length2, 0.0f, 1.0f) : 0.0f;
    const ImVec2 closest = a + ab * t;
    return std::sqrt((p.x - closest.x) * (p.x - closest.x) + (p.y - closest.y) * (p.y - closest.y));
}

ImU32 Brighter(ImU32 colour, float amount)
{
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(colour);
    value.x = value.x + (1.0f - value.x) * amount;
    value.y = value.y + (1.0f - value.y) * amount;
    value.z = value.z + (1.0f - value.z) * amount;
    return ImGui::ColorConvertFloat4ToU32(value);
}

bool WouldCreateCycle(const MaterialShaderGraph& graph, uint32_t fromNodeId, uint32_t toNodeId)
{
    std::vector<uint32_t> pending{toNodeId};
    std::set<uint32_t> visited;
    while (!pending.empty())
    {
        const uint32_t current = pending.back();
        pending.pop_back();
        if (current == fromNodeId)
        {
            return true;
        }
        if (!visited.insert(current).second)
        {
            continue;
        }
        for (const MaterialShaderLink& link : graph.links)
        {
            if (link.fromNodeId == current)
            {
                pending.push_back(link.toNodeId);
            }
        }
    }
    return false;
}

std::string BuildNodeName(MaterialShaderNodeType type, uint32_t nodeId)
{
    if (type == MaterialShaderNodeType::Output)
    {
        return "Material Output";
    }
    return std::string(GetMaterialGraphNodeTypeLabel(type)) + " " + std::to_string(nodeId);
}
}

// ---------------------------------------------------------------------------------------------------
// Nodes and pins
// ---------------------------------------------------------------------------------------------------

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
        {"base_factor", "Base Color", MaterialGraphPinKind::Color},
        {"metallic_factor", "Metallic", MaterialGraphPinKind::Scalar},
        {"roughness_factor", "Roughness", MaterialGraphPinKind::Scalar},
        {"normal_scale", "Normal Scale", MaterialGraphPinKind::Scalar},
        {"ao_strength", "AO Strength", MaterialGraphPinKind::Scalar},
        {"emissive_color", "Emissive Color", MaterialGraphPinKind::Color},
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
    static const std::vector<MaterialGraphPinDefinition> kTexturePins = {{"texture", "Texture", MaterialGraphPinKind::Texture}};
    static const std::vector<MaterialGraphPinDefinition> kScalarPins = {{"value", "Value", MaterialGraphPinKind::Scalar}};
    static const std::vector<MaterialGraphPinDefinition> kColorPins = {{"color", "Color", MaterialGraphPinKind::Color}};
    static const std::vector<MaterialGraphPinDefinition> kSurfacePins = {{"surface", "Surface", MaterialGraphPinKind::Surface}};
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

const MaterialGraphPinDefinition* FindMaterialGraphPin(MaterialShaderNodeType type, std::string_view slot, bool input)
{
    const auto& pins = input ? GetMaterialGraphInputPins(type) : GetMaterialGraphOutputPins(type);
    const auto found = std::find_if(pins.begin(), pins.end(), [slot](const MaterialGraphPinDefinition& pin)
                                    {
                                        return pin.slot == slot;
                                    });
    return found != pins.end() ? &*found : nullptr;
}

std::span<const MaterialGraphNodeKind> MaterialGraphNodeKinds()
{
    static const MaterialGraphNodeKind kKinds[] = {
        {MaterialShaderNodeType::Texture, "Texture", "Inputs", "A texture file: one of a Surface's maps, or a Blend's mask."},
        {MaterialShaderNodeType::Scalar, "Scalar", "Inputs", "A number: a Blend's factor, or one of the Output's numbers (metallic, roughness, ...)."},
        {MaterialShaderNodeType::Color, "Color", "Inputs", "A colour: the Output's base colour or emissive colour."},
        {MaterialShaderNodeType::Surface, "Surface", "Surfaces", "A surface's maps together: base colour, normal, metallic, roughness, occlusion and emissive."},
        {MaterialShaderNodeType::Blend, "Blend", "Surfaces", "Two surfaces mixed by a factor, where a mask says."},
        {MaterialShaderNodeType::Output, "Output", "Material", "Where the material comes out: its surface and every factor. A graph has one."}};
    return kKinds;
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

std::string MaterialGraphNodeTitle(const MaterialShaderNode& node)
{
    if (!node.name.empty())
    {
        return node.name;
    }
    return node.type == MaterialShaderNodeType::Output ? "Material Output" : GetMaterialGraphNodeTypeLabel(node.type);
}

ImU32 GetMaterialGraphPinColor(MaterialGraphPinKind kind)
{
    switch (kind)
    {
    case MaterialGraphPinKind::Texture:
        return IM_COL32(90, 160, 245, 255);
    case MaterialGraphPinKind::Scalar:
        return IM_COL32(140, 215, 90, 255);
    case MaterialGraphPinKind::Color:
        return IM_COL32(240, 196, 70, 255);
    case MaterialGraphPinKind::Surface:
        return IM_COL32(230, 110, 180, 255);
    default:
        return IM_COL32(200, 200, 200, 255);
    }
}

ImU32 GetMaterialGraphHeaderColor(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return IM_COL32(38, 72, 125, 255);
    case MaterialShaderNodeType::Scalar:
        return IM_COL32(62, 100, 44, 255);
    case MaterialShaderNodeType::Color:
        return IM_COL32(120, 92, 34, 255);
    case MaterialShaderNodeType::Surface:
        return IM_COL32(40, 96, 96, 255);
    case MaterialShaderNodeType::Blend:
        return IM_COL32(92, 56, 110, 255);
    case MaterialShaderNodeType::Output:
        return IM_COL32(112, 58, 40, 255);
    default:
        return IM_COL32(60, 60, 64, 255);
    }
}

const MaterialShaderNode* FindMaterialGraphNode(const MaterialShaderGraph& graph, uint32_t nodeId)
{
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [nodeId](const MaterialShaderNode& node)
                                    {
                                        return node.id == nodeId;
                                    });
    return found != graph.nodes.end() ? &*found : nullptr;
}

MaterialShaderNode* FindMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId)
{
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [nodeId](const MaterialShaderNode& node)
                                    {
                                        return node.id == nodeId;
                                    });
    return found != graph.nodes.end() ? &*found : nullptr;
}

const MaterialShaderLink* FindMaterialGraphLink(const MaterialShaderGraph& graph, uint32_t linkId)
{
    const auto found = std::find_if(graph.links.begin(), graph.links.end(), [linkId](const MaterialShaderLink& link)
                                    {
                                        return link.id == linkId;
                                    });
    return found != graph.links.end() ? &*found : nullptr;
}

const MaterialShaderLink* FindIncomingMaterialGraphLink(const MaterialShaderGraph& graph, uint32_t nodeId, std::string_view slot)
{
    const auto found = std::find_if(graph.links.begin(), graph.links.end(), [&](const MaterialShaderLink& link)
                                    {
                                        return link.toNodeId == nodeId && link.toSlot == slot;
                                    });
    return found != graph.links.end() ? &*found : nullptr;
}

MaterialShaderNode* FindMaterialGraphOutputNode(MaterialShaderGraph& graph)
{
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const MaterialShaderNode& node)
                                    {
                                        return node.type == MaterialShaderNodeType::Output;
                                    });
    return found != graph.nodes.end() ? &*found : nullptr;
}

bool MaterialGraphHasOutputNode(const MaterialShaderGraph& graph)
{
    return std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const MaterialShaderNode& node)
                       {
                           return node.type == MaterialShaderNodeType::Output;
                       });
}

MaterialShaderNode* AddMaterialGraphNode(MaterialShaderGraph& graph, MaterialShaderNodeType type, const MaterialGraphNodePosition& position)
{
    MaterialShaderNode node{};
    node.id = graph.nextNodeId++;
    node.type = type;
    node.name = BuildNodeName(type, node.id);
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
    std::erase_if(graph.links, [linkId](const MaterialShaderLink& link)
                  {
                      return link.id == linkId;
                  });
}

void RemoveMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId)
{
    std::erase_if(graph.links, [nodeId](const MaterialShaderLink& link)
                  {
                      return link.fromNodeId == nodeId || link.toNodeId == nodeId;
                  });
    std::erase_if(graph.nodes, [nodeId](const MaterialShaderNode& node)
                  {
                      return node.id == nodeId;
                  });
}

bool BreakMaterialGraphPinLinks(MaterialShaderGraph& graph, uint32_t nodeId, std::string_view slot, bool input)
{
    return std::erase_if(graph.links, [&](const MaterialShaderLink& link)
                         {
                             return input ? (link.toNodeId == nodeId && link.toSlot == slot) : (link.fromNodeId == nodeId && link.fromSlot == slot);
                         }) > 0;
}

bool CanConnectMaterialGraphPins(
    const MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    std::string_view fromSlot,
    uint32_t toNodeId,
    std::string_view toSlot,
    std::string* failureReason)
{
    const auto fail = [failureReason](const char* reason)
    {
        if (failureReason != nullptr)
        {
            *failureReason = reason;
        }
        return false;
    };
    const MaterialShaderNode* fromNode = FindMaterialGraphNode(graph, fromNodeId);
    const MaterialShaderNode* toNode = FindMaterialGraphNode(graph, toNodeId);
    if (fromNode == nullptr || toNode == nullptr)
    {
        return fail("The pin's node is gone.");
    }
    if (fromNodeId == toNodeId)
    {
        return fail("A node cannot feed itself.");
    }
    const MaterialGraphPinDefinition* outputPin = FindMaterialGraphPin(fromNode->type, fromSlot, false);
    const MaterialGraphPinDefinition* inputPin = FindMaterialGraphPin(toNode->type, toSlot, true);
    if (outputPin == nullptr || inputPin == nullptr)
    {
        return fail("Links run from an output pin to an input pin.");
    }
    if (outputPin->kind != inputPin->kind)
    {
        return fail("Only pins of the same kind connect (their colours match).");
    }
    if (WouldCreateCycle(graph, fromNodeId, toNodeId))
    {
        return fail("That link would make a loop.");
    }
    return true;
}

uint32_t ConnectMaterialGraphPins(MaterialShaderGraph& graph, uint32_t fromNodeId, std::string_view fromSlot, uint32_t toNodeId, std::string_view toSlot)
{
    if (!CanConnectMaterialGraphPins(graph, fromNodeId, fromSlot, toNodeId, toSlot))
    {
        return 0;
    }
    for (const MaterialShaderLink& existing : graph.links)
    {
        if (existing.fromNodeId == fromNodeId && existing.fromSlot == fromSlot && existing.toNodeId == toNodeId && existing.toSlot == toSlot)
        {
            return existing.id;
        }
    }
    BreakMaterialGraphPinLinks(graph, toNodeId, toSlot, true);
    MaterialShaderLink link{};
    link.id = graph.nextLinkId++;
    link.fromNodeId = fromNodeId;
    link.fromSlot = std::string(fromSlot);
    link.toNodeId = toNodeId;
    link.toSlot = std::string(toSlot);
    graph.links.push_back(link);
    return link.id;
}

MaterialGraphClipboard CopyMaterialGraphNodes(const MaterialShaderGraph& graph, const std::set<uint32_t>& nodeIds)
{
    MaterialGraphClipboard clipboard;
    std::set<uint32_t> copied;
    for (const MaterialShaderNode& node : graph.nodes)
    {
        if (nodeIds.contains(node.id) && node.type != MaterialShaderNodeType::Output)
        {
            clipboard.nodes.push_back(node);
            copied.insert(node.id);
        }
    }
    for (const MaterialShaderLink& link : graph.links)
    {
        if (copied.contains(link.fromNodeId) && copied.contains(link.toNodeId))
        {
            clipboard.links.push_back(link);
        }
    }
    return clipboard;
}

std::vector<uint32_t> PasteMaterialGraphNodes(MaterialShaderGraph& graph, const MaterialGraphClipboard& clipboard, const MaterialGraphNodePosition& at)
{
    std::vector<uint32_t> pasted;
    if (clipboard.IsEmpty())
    {
        return pasted;
    }
    MaterialGraphNodePosition topLeft{FLT_MAX, FLT_MAX};
    for (const MaterialShaderNode& node : clipboard.nodes)
    {
        topLeft.x = std::min(topLeft.x, node.position.x);
        topLeft.y = std::min(topLeft.y, node.position.y);
    }
    std::unordered_map<uint32_t, uint32_t> newIds;
    for (const MaterialShaderNode& source : clipboard.nodes)
    {
        MaterialShaderNode node = source;
        node.id = graph.nextNodeId++;
        node.position = MaterialGraphNodePosition{at.x + (source.position.x - topLeft.x), at.y + (source.position.y - topLeft.y)};
        newIds[source.id] = node.id;
        pasted.push_back(node.id);
        graph.nodes.push_back(std::move(node));
    }
    for (const MaterialShaderLink& source : clipboard.links)
    {
        MaterialShaderLink link = source;
        link.id = graph.nextLinkId++;
        link.fromNodeId = newIds[source.fromNodeId];
        link.toNodeId = newIds[source.toNodeId];
        graph.links.push_back(std::move(link));
    }
    return pasted;
}

bool RepairMaterialGraphLinks(MaterialShaderGraph& graph)
{
    const size_t before = graph.links.size();
    std::erase_if(graph.links, [&](const MaterialShaderLink& link)
                  {
                      return FindMaterialGraphNode(graph, link.fromNodeId) == nullptr || FindMaterialGraphNode(graph, link.toNodeId) == nullptr;
                  });
    bool changed = graph.links.size() != before;
    const MaterialShaderNode* output = FindMaterialGraphOutputNode(graph);
    if (output == nullptr || FindIncomingMaterialGraphLink(graph, output->id, "surface") != nullptr)
    {
        return changed;
    }
    const MaterialShaderNode* loose = nullptr;
    int looseCount = 0;
    for (const MaterialShaderNode& node : graph.nodes)
    {
        if (node.type != MaterialShaderNodeType::Surface && node.type != MaterialShaderNodeType::Blend)
        {
            continue;
        }
        const bool feeds = std::any_of(graph.links.begin(), graph.links.end(), [&](const MaterialShaderLink& link)
                                       {
                                           return link.fromNodeId == node.id;
                                       });
        if (!feeds)
        {
            loose = &node;
            ++looseCount;
        }
    }
    if (looseCount == 1)
    {
        const uint32_t from = loose->id;
        const uint32_t to = output->id;
        changed |= ConnectMaterialGraphPins(graph, from, "surface", to, "surface") != 0;
    }
    return changed;
}

ImVec2 MaterialGraphNodeSize(const MaterialShaderNode& node)
{
    const NodeMetrics metrics = MeasureNode(node, 1.0f, BaseFontSize());
    return ImVec2(metrics.width, metrics.height);
}

void ArrangeMaterialGraph(MaterialShaderGraph& graph)
{
    if (graph.nodes.empty())
    {
        return;
    }
    // Each node's column: one left of the farthest node it feeds; the Output's is 0.
    std::unordered_map<uint32_t, int> column;
    for (const MaterialShaderNode& node : graph.nodes)
    {
        if (node.type == MaterialShaderNodeType::Output)
        {
            column[node.id] = 0;
        }
    }
    for (size_t pass = 0; pass < graph.nodes.size(); ++pass)
    {
        bool moved = false;
        for (const MaterialShaderLink& link : graph.links)
        {
            const auto to = column.find(link.toNodeId);
            if (to == column.end())
            {
                continue;
            }
            int& from = column.try_emplace(link.fromNodeId, -1).first->second;
            if (from < to->second + 1)
            {
                from = to->second + 1;
                moved = true;
            }
        }
        if (!moved)
        {
            break;
        }
    }
    int deepest = 0;
    for (const auto& [id, value] : column)
    {
        deepest = std::max(deepest, value);
    }
    // Nodes that feed nothing on the way to the Output: a column of their own at the left.
    for (const MaterialShaderNode& node : graph.nodes)
    {
        if (!column.contains(node.id) || column[node.id] < 0)
        {
            column[node.id] = deepest + 1;
        }
    }

    std::map<int, std::vector<MaterialShaderNode*>> columns;
    for (MaterialShaderNode& node : graph.nodes)
    {
        columns[column[node.id]].push_back(&node);
    }
    constexpr float kColumnGap = 90.0f;
    constexpr float kRowGap = 28.0f;
    const MaterialShaderNode* output = FindMaterialGraphOutputNode(graph);
    const float anchorX = output != nullptr ? output->position.x : 0.0f;
    const float anchorY = output != nullptr ? output->position.y : 0.0f;
    // Where each node sits in its column's order, which orders the column left of it.
    std::unordered_map<uint32_t, float> order;
    float right = anchorX + (output != nullptr ? MaterialGraphNodeSize(*output).x : 0.0f);
    float previousCentreY = anchorY;
    for (auto& [index, nodes] : columns)
    {
        // A node sorts by the first pin it feeds, in the order of the column it feeds.
        const auto key = [&](const MaterialShaderNode* node)
        {
            float best = FLT_MAX;
            for (const MaterialShaderLink& link : graph.links)
            {
                if (link.fromNodeId != node->id || !order.contains(link.toNodeId))
                {
                    continue;
                }
                const MaterialShaderNode* to = FindMaterialGraphNode(graph, link.toNodeId);
                const auto& pins = GetMaterialGraphInputPins(to->type);
                const auto pin = std::find_if(pins.begin(), pins.end(), [&](const MaterialGraphPinDefinition& definition)
                                              {
                                                  return definition.slot == link.toSlot;
                                              });
                best = std::min(best, order[link.toNodeId] * 100.0f + static_cast<float>(pin - pins.begin()));
            }
            return best;
        };
        std::stable_sort(nodes.begin(), nodes.end(), [&](const MaterialShaderNode* a, const MaterialShaderNode* b)
                         {
                             return key(a) < key(b);
                         });
        float width = 0.0f;
        float height = 0.0f;
        for (const MaterialShaderNode* node : nodes)
        {
            const ImVec2 size = MaterialGraphNodeSize(*node);
            width = std::max(width, size.x);
            height += size.y + kRowGap;
        }
        height -= kRowGap;
        const float x = right - width - (index == 0 ? 0.0f : kColumnGap);
        float y = previousCentreY - height * 0.5f;
        for (size_t row = 0; row < nodes.size(); ++row)
        {
            MaterialShaderNode* node = nodes[row];
            const ImVec2 size = MaterialGraphNodeSize(*node);
            node->position = MaterialGraphNodePosition{x + (width - size.x), y};
            order[node->id] = static_cast<float>(row);
            y += size.y + kRowGap;
        }
        right = x;
        previousCentreY = previousCentreY;
    }
}

// ---------------------------------------------------------------------------------------------------
// The canvas
// ---------------------------------------------------------------------------------------------------

ImVec2 MaterialGraphCanvas::ToScreen(const MaterialGraphNodePosition& position) const
{
    return ImVec2(m_canvasMin.x + (position.x - m_view.origin.x) * m_scale, m_canvasMin.y + (position.y - m_view.origin.y) * m_scale);
}

MaterialGraphNodePosition MaterialGraphCanvas::ToGraph(const ImVec2& screen) const
{
    return MaterialGraphNodePosition{m_view.origin.x + (screen.x - m_canvasMin.x) / m_scale, m_view.origin.y + (screen.y - m_canvasMin.y) / m_scale};
}

void MaterialGraphCanvas::Layout(const MaterialShaderGraph& graph, const ImVec2& canvasMin, const MaterialGraphView& view, float scale, float fontSize)
{
    static_cast<void>(canvasMin);
    static_cast<void>(view);
    m_layouts.clear();
    m_layouts.reserve(graph.nodes.size());
    for (const MaterialShaderNode& node : graph.nodes)
    {
        const NodeMetrics metrics = MeasureNode(node, scale, fontSize);
        NodeLayout layout;
        layout.nodeId = node.id;
        layout.min = ToScreen(node.position);
        layout.max = layout.min + ImVec2(metrics.width, metrics.height);
        layout.headerHeight = metrics.headerHeight;
        const float bodyTop = layout.min.y + metrics.headerHeight + metrics.padding * 0.5f;
        const auto addPins = [&](const std::vector<MaterialGraphPinDefinition>& pins, bool input)
        {
            for (size_t row = 0; row < pins.size(); ++row)
            {
                PinLayout pin;
                pin.definition = &pins[row];
                pin.input = input;
                const float y = bodyTop + (static_cast<float>(row) + 0.5f) * metrics.rowHeight;
                pin.centre = ImVec2(input ? layout.min.x : layout.max.x, y);
                const float label = TextWidth(fontSize, PinText(node, pins[row], input).c_str());
                const float halfRow = metrics.rowHeight * 0.5f;
                if (input)
                {
                    pin.hitMin = ImVec2(layout.min.x - metrics.pinRadius * 2.5f, y - halfRow);
                    pin.hitMax = ImVec2(layout.min.x + metrics.padding + metrics.pinRadius * 2.0f + label, y + halfRow);
                }
                else
                {
                    pin.hitMin = ImVec2(layout.max.x - metrics.padding - metrics.pinRadius * 2.0f - label, y - halfRow);
                    pin.hitMax = ImVec2(layout.max.x + metrics.pinRadius * 2.5f, y + halfRow);
                }
                for (const MaterialShaderLink& link : graph.links)
                {
                    if (input ? (link.toNodeId == node.id && link.toSlot == pins[row].slot) : (link.fromNodeId == node.id && link.fromSlot == pins[row].slot))
                    {
                        pin.linked = true;
                        break;
                    }
                }
                layout.pins.push_back(pin);
            }
        };
        addPins(GetMaterialGraphInputPins(node.type), true);
        addPins(GetMaterialGraphOutputPins(node.type), false);
        m_layouts.push_back(std::move(layout));
    }
}

const MaterialGraphCanvas::NodeLayout* MaterialGraphCanvas::NodeAt(const ImVec2& point) const
{
    // Drawn last is on top.
    for (auto layout = m_layouts.rbegin(); layout != m_layouts.rend(); ++layout)
    {
        if (point.x >= layout->min.x && point.y >= layout->min.y && point.x < layout->max.x && point.y < layout->max.y)
        {
            return &*layout;
        }
    }
    return nullptr;
}

const MaterialGraphCanvas::PinLayout* MaterialGraphCanvas::PinAt(const ImVec2& point, const NodeLayout** owner) const
{
    for (auto layout = m_layouts.rbegin(); layout != m_layouts.rend(); ++layout)
    {
        for (const PinLayout& pin : layout->pins)
        {
            if (point.x >= pin.hitMin.x && point.y >= pin.hitMin.y && point.x < pin.hitMax.x && point.y < pin.hitMax.y)
            {
                if (owner != nullptr)
                {
                    *owner = &*layout;
                }
                return &pin;
            }
        }
        // A node above another hides the other's pins.
        if (point.x >= layout->min.x && point.y >= layout->min.y && point.x < layout->max.x && point.y < layout->max.y)
        {
            return nullptr;
        }
    }
    return nullptr;
}

const MaterialGraphCanvas::PinLayout* MaterialGraphCanvas::FindPin(uint32_t nodeId, std::string_view slot, bool input) const
{
    for (const NodeLayout& layout : m_layouts)
    {
        if (layout.nodeId != nodeId)
        {
            continue;
        }
        for (const PinLayout& pin : layout.pins)
        {
            if (pin.input == input && pin.definition->slot == slot)
            {
                return &pin;
            }
        }
    }
    return nullptr;
}

uint32_t MaterialGraphCanvas::LinkAt(const MaterialShaderGraph& graph, const ImVec2& point) const
{
    const float tolerance = 5.0f * std::max(m_scale, 0.6f);
    uint32_t best = 0;
    float bestDistance = tolerance;
    for (const MaterialShaderLink& link : graph.links)
    {
        const PinLayout* from = FindPin(link.fromNodeId, link.fromSlot, false);
        const PinLayout* to = FindPin(link.toNodeId, link.toSlot, true);
        if (from == nullptr || to == nullptr)
        {
            continue;
        }
        ImVec2 c0;
        ImVec2 c1;
        LinkControls(from->centre, to->centre, m_scale, c0, c1);
        ImVec2 previous = from->centre;
        constexpr int kSegments = 24;
        for (int segment = 1; segment <= kSegments; ++segment)
        {
            const ImVec2 next = ImBezierCubicCalc(from->centre, c0, c1, to->centre, static_cast<float>(segment) / kSegments);
            const float distance = DistanceToSegment(point, previous, next);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = link.id;
            }
            previous = next;
        }
    }
    return best;
}

MaterialGraphCanvas::Result MaterialGraphCanvas::Draw(
    MaterialShaderGraph& graph, MaterialGraphView& view, MaterialGraphSelection& selection, const Context& context)
{
    Result result;
    // A selection of nodes or a link that are gone (an undo, another slot) is dropped.
    std::erase_if(selection.nodes, [&](uint32_t id)
                  {
                      return FindMaterialGraphNode(graph, id) == nullptr;
                  });
    if (selection.link != 0 && FindMaterialGraphLink(graph, selection.link) == nullptr)
    {
        selection.link = 0;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    m_canvasMin = ImGui::GetCursorScreenPos();
    m_canvasMax = m_canvasMin + ImVec2(std::max(available.x, 1.0f), std::max(available.y, 1.0f));
    // One button over the whole canvas takes every mouse button, so a drag on the background never
    // moves the window and the canvas is the hovered item wherever the mouse is on it.
    ImGui::InvisibleButton(
        "##material_graph_canvas",
        m_canvasMax - m_canvasMin,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
    m_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    m_fontSize = ImGui::GetFontSize();
    ApplyFraming(graph, view);
    m_view = view;
    m_scale = context.uiScale * view.zoom;
    Layout(graph, m_canvasMin, view, m_scale, m_fontSize * view.zoom);

    // Selected nodes are drawn above the rest.
    std::stable_partition(m_layouts.begin(), m_layouts.end(), [&](const NodeLayout& layout)
                          {
                              return !selection.nodes.contains(layout.nodeId);
                          });

    HandleMouse(graph, view, selection, m_hovered, result);
    if (result.changed)
    {
        m_view = view;
        m_scale = context.uiScale * view.zoom;
        Layout(graph, m_canvasMin, view, m_scale, m_fontSize * view.zoom);
        std::stable_partition(m_layouts.begin(), m_layouts.end(), [&](const NodeLayout& layout)
                              {
                                  return !selection.nodes.contains(layout.nodeId);
                              });
    }
    else if (view.origin.x != m_view.origin.x || view.origin.y != m_view.origin.y || view.zoom != m_view.zoom)
    {
        m_view = view;
        m_scale = context.uiScale * view.zoom;
        Layout(graph, m_canvasMin, view, m_scale, m_fontSize * view.zoom);
        std::stable_partition(m_layouts.begin(), m_layouts.end(), [&](const NodeLayout& layout)
                              {
                                  return !selection.nodes.contains(layout.nodeId);
                              });
    }

    ImDrawList& drawList = *ImGui::GetWindowDrawList();
    drawList.PushClipRect(m_canvasMin, m_canvasMax, true);
    DrawGrid(drawList, view);
    DrawLinks(drawList, graph, selection);
    DrawNodes(drawList, graph, selection, context);
    DrawOverlay(drawList, view);
    drawList.PopClipRect();

    DrawMenus(graph, selection, result);
    if (result.changed)
    {
        std::erase_if(selection.nodes, [&](uint32_t id)
                      {
                          return FindMaterialGraphNode(graph, id) == nullptr;
                      });
    }
    result.dragging = m_mode == Mode::DraggingNodes && m_dragMoved;
    if (context.statusMessage != nullptr && m_mode == Mode::DraggingLink)
    {
        *context.statusMessage = "Drop on a pin of the same colour, or on the background for a node that fits.";
    }
    return result;
}

void MaterialGraphCanvas::ApplyFraming(const MaterialShaderGraph& graph, MaterialGraphView& view)
{
    if (!view.frameAll && !m_frameRequest.has_value())
    {
        return;
    }
    const std::set<uint32_t> nodes = m_frameRequest.value_or(std::set<uint32_t>{});
    m_frameRequest.reset();
    view.frameAll = false;
    ImVec2 minimum(FLT_MAX, FLT_MAX);
    ImVec2 maximum(-FLT_MAX, -FLT_MAX);
    for (const MaterialShaderNode& node : graph.nodes)
    {
        if (!nodes.empty() && !nodes.contains(node.id))
        {
            continue;
        }
        const ImVec2 size = MaterialGraphNodeSize(node);
        minimum.x = std::min(minimum.x, node.position.x);
        minimum.y = std::min(minimum.y, node.position.y);
        maximum.x = std::max(maximum.x, node.position.x + size.x);
        maximum.y = std::max(maximum.y, node.position.y + size.y);
    }
    if (minimum.x > maximum.x)
    {
        return;
    }
    const float uiScale = std::max(ImGui::GetStyle().FontScaleMain, 0.1f);
    const ImVec2 canvas = m_canvasMax - m_canvasMin;
    constexpr float kMargin = 60.0f;
    const float zoomX = canvas.x / (uiScale * (maximum.x - minimum.x + 2.0f * kMargin));
    const float zoomY = canvas.y / (uiScale * (maximum.y - minimum.y + 2.0f * kMargin));
    view.zoom = std::clamp(std::min(zoomX, zoomY), kMaterialGraphMinZoom, 1.0f);
    const float visibleWidth = canvas.x / (uiScale * view.zoom);
    const float visibleHeight = canvas.y / (uiScale * view.zoom);
    view.origin.x = 0.5f * (minimum.x + maximum.x) - 0.5f * visibleWidth;
    view.origin.y = 0.5f * (minimum.y + maximum.y) - 0.5f * visibleHeight;
}

void MaterialGraphCanvas::HandleMouse(MaterialShaderGraph& graph, MaterialGraphView& view, MaterialGraphSelection& selection, bool hovered, Result& result)
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float uiScale = std::max(ImGui::GetStyle().FontScaleMain, 0.1f);

    // The wheel zooms about the cursor, in steps as Unreal's graph does.
    if (hovered && io.MouseWheel != 0.0f && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    {
        const MaterialGraphNodePosition anchor = ToGraph(mouse);
        view.zoom = std::clamp(view.zoom * std::pow(1.15f, io.MouseWheel), kMaterialGraphMinZoom, kMaterialGraphMaxZoom);
        // Snap close to 1:1 onto it.
        if (std::abs(view.zoom - 1.0f) < 0.04f)
        {
            view.zoom = 1.0f;
        }
        const float scale = uiScale * view.zoom;
        view.origin.x = anchor.x - (mouse.x - m_canvasMin.x) / scale;
        view.origin.y = anchor.y - (mouse.y - m_canvasMin.y) / scale;
    }

    switch (m_mode)
    {
    case Mode::None:
    {
        if (!hovered)
        {
            break;
        }
        const NodeLayout* owner = nullptr;
        const PinLayout* pin = PinAt(mouse, &owner);
        const NodeLayout* node = pin == nullptr ? NodeAt(mouse) : nullptr;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_pressPosition = mouse;
            if (pin != nullptr)
            {
                if (io.KeyAlt)
                {
                    result.changed |= BreakMaterialGraphPinLinks(graph, owner->nodeId, pin->definition->slot, pin->input);
                    break;
                }
                const MaterialShaderLink* incoming = pin->input ? FindIncomingMaterialGraphLink(graph, owner->nodeId, pin->definition->slot) : nullptr;
                if (incoming != nullptr && io.KeyCtrl)
                {
                    // Ctrl takes the input's link to wherever it is dropped.
                    const MaterialShaderNode* fromNode = FindMaterialGraphNode(graph, incoming->fromNodeId);
                    const MaterialGraphPinDefinition* fromPin =
                        fromNode != nullptr ? FindMaterialGraphPin(fromNode->type, incoming->fromSlot, false) : nullptr;
                    if (fromPin != nullptr)
                    {
                        m_linkFrom = PinRef{incoming->fromNodeId, incoming->fromSlot, false, fromPin->kind};
                        RemoveMaterialGraphLink(graph, incoming->id);
                        result.changed = true;
                        m_mode = Mode::DraggingLink;
                    }
                    break;
                }
                m_linkFrom = PinRef{owner->nodeId, pin->definition->slot, pin->input, pin->definition->kind};
                m_mode = Mode::DraggingLink;
                break;
            }
            if (node != nullptr)
            {
                const uint32_t id = node->nodeId;
                selection.link = 0;
                m_clickSelectsOnly = 0;
                if (io.KeyCtrl)
                {
                    if (!selection.nodes.erase(id))
                    {
                        selection.nodes.insert(id);
                    }
                    break;
                }
                if (io.KeyShift)
                {
                    selection.nodes.insert(id);
                }
                else if (!selection.nodes.contains(id))
                {
                    selection.nodes = {id};
                }
                else if (selection.nodes.size() > 1)
                {
                    m_clickSelectsOnly = id;
                }
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    const MaterialShaderNode* clicked = FindMaterialGraphNode(graph, id);
                    if (clicked != nullptr && clicked->type == MaterialShaderNodeType::Texture)
                    {
                        result.pickTextureForNode = id;
                    }
                }
                m_mode = Mode::DraggingNodes;
                m_dragMoved = false;
                m_dragMouseStart = ToGraph(mouse);
                m_dragStart.clear();
                for (const uint32_t selected : selection.nodes)
                {
                    if (const MaterialShaderNode* dragged = FindMaterialGraphNode(graph, selected))
                    {
                        m_dragStart.emplace_back(selected, dragged->position);
                    }
                }
                break;
            }
            const uint32_t link = LinkAt(graph, mouse);
            if (link != 0)
            {
                if (io.KeyAlt)
                {
                    RemoveMaterialGraphLink(graph, link);
                    result.changed = true;
                    break;
                }
                if (!io.KeyCtrl && !io.KeyShift)
                {
                    selection.nodes.clear();
                }
                selection.link = link;
                break;
            }
            // The background: a box selection, or a click that clears the selection.
            m_mode = Mode::BoxSelecting;
            m_boxSubtract = io.KeyCtrl;
            m_boxBase = io.KeyCtrl || io.KeyShift ? selection.nodes : std::set<uint32_t>{};
            if (!io.KeyCtrl && !io.KeyShift)
            {
                selection.nodes.clear();
                selection.link = 0;
            }
            break;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_mode = Mode::RightPressed;
            m_pressPosition = mouse;
            break;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            m_mode = Mode::Panning;
            m_pressPosition = mouse;
        }
        break;
    }
    case Mode::RightPressed:
    {
        const float travel = std::abs(mouse.x - m_pressPosition.x) + std::abs(mouse.y - m_pressPosition.y);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && travel > 4.0f * uiScale)
        {
            m_mode = Mode::Panning;
        }
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            // A right click: the menu for what is under it.
            m_mode = Mode::None;
            m_menuPosition = ToGraph(m_pressPosition);
            const NodeLayout* owner = nullptr;
            const PinLayout* pin = PinAt(m_pressPosition, &owner);
            const NodeLayout* node = pin == nullptr ? NodeAt(m_pressPosition) : nullptr;
            const uint32_t link = pin == nullptr && node == nullptr ? LinkAt(graph, m_pressPosition) : 0;
            if (pin != nullptr)
            {
                m_menuPin = PinRef{owner->nodeId, pin->definition->slot, pin->input, pin->definition->kind};
                m_menuRequest = MenuKind::Pin;
            }
            else if (node != nullptr)
            {
                m_menuNode = node->nodeId;
                if (!selection.nodes.contains(node->nodeId))
                {
                    selection.nodes = {node->nodeId};
                    selection.link = 0;
                }
                m_menuRequest = MenuKind::Node;
            }
            else if (link != 0)
            {
                m_menuLink = link;
                selection.nodes.clear();
                selection.link = link;
                m_menuRequest = MenuKind::Link;
            }
            else
            {
                m_menuRequest = MenuKind::Background;
            }
        }
        break;
    }
    case Mode::Panning:
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            const float scale = uiScale * view.zoom;
            view.origin.x -= io.MouseDelta.x / scale;
            view.origin.y -= io.MouseDelta.y / scale;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        else
        {
            m_mode = Mode::None;
        }
        break;
    }
    case Mode::DraggingNodes:
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (!m_dragMoved && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                m_dragMoved = true;
            }
            if (m_dragMoved)
            {
                const MaterialGraphNodePosition now = ToGraph(mouse);
                const float dx = now.x - m_dragMouseStart.x;
                const float dy = now.y - m_dragMouseStart.y;
                for (const auto& [id, start] : m_dragStart)
                {
                    if (MaterialShaderNode* dragged = FindMaterialGraphNode(graph, id))
                    {
                        const MaterialGraphNodePosition moved{start.x + dx, start.y + dy};
                        if (moved.x != dragged->position.x || moved.y != dragged->position.y)
                        {
                            dragged->position = moved;
                            result.changed = true;
                        }
                    }
                }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        }
        else
        {
            if (!m_dragMoved && m_clickSelectsOnly != 0)
            {
                selection.nodes = {m_clickSelectsOnly};
            }
            m_clickSelectsOnly = 0;
            m_mode = Mode::None;
        }
        break;
    }
    case Mode::BoxSelecting:
    {
        const ImVec2 boxMin(std::min(m_pressPosition.x, mouse.x), std::min(m_pressPosition.y, mouse.y));
        const ImVec2 boxMax(std::max(m_pressPosition.x, mouse.x), std::max(m_pressPosition.y, mouse.y));
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            {
                selection.nodes = m_boxBase;
                for (const NodeLayout& layout : m_layouts)
                {
                    const bool inside = layout.min.x < boxMax.x && layout.max.x > boxMin.x && layout.min.y < boxMax.y && layout.max.y > boxMin.y;
                    if (!inside)
                    {
                        continue;
                    }
                    if (m_boxSubtract)
                    {
                        selection.nodes.erase(layout.nodeId);
                    }
                    else
                    {
                        selection.nodes.insert(layout.nodeId);
                    }
                }
            }
        }
        else
        {
            m_mode = Mode::None;
        }
        break;
    }
    case Mode::DraggingLink:
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            FinishLinkDrag(graph, selection, result);
            m_mode = Mode::None;
        }
        break;
    }
    }
}

void MaterialGraphCanvas::FinishLinkDrag(MaterialShaderGraph& graph, MaterialGraphSelection& selection, Result& result)
{
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto connect = [&](uint32_t nodeId, std::string_view slot, bool input) -> bool
    {
        if (input == m_linkFrom.input)
        {
            return false;
        }
        const uint32_t fromNode = m_linkFrom.input ? nodeId : m_linkFrom.nodeId;
        const std::string_view fromSlot = m_linkFrom.input ? slot : std::string_view(m_linkFrom.slot);
        const uint32_t toNode = m_linkFrom.input ? m_linkFrom.nodeId : nodeId;
        const std::string_view toSlot = m_linkFrom.input ? std::string_view(m_linkFrom.slot) : slot;
        std::string reason;
        if (!CanConnectMaterialGraphPins(graph, fromNode, fromSlot, toNode, toSlot, &reason))
        {
            return false;
        }
        const uint32_t link = ConnectMaterialGraphPins(graph, fromNode, fromSlot, toNode, toSlot);
        if (link != 0)
        {
            selection.link = link;
            result.changed = true;
        }
        return link != 0;
    };
    const NodeLayout* owner = nullptr;
    if (const PinLayout* pin = PinAt(mouse, &owner))
    {
        connect(owner->nodeId, pin->definition->slot, pin->input);
        return;
    }
    if (const NodeLayout* node = NodeAt(mouse))
    {
        // Dropped on a node: its first pin that fits.
        for (const PinLayout& pin : node->pins)
        {
            if (pin.definition->kind == m_linkFrom.kind && pin.input != m_linkFrom.input && connect(node->nodeId, pin.definition->slot, pin.input))
            {
                return;
            }
        }
        return;
    }
    if (mouse.x >= m_canvasMin.x && mouse.y >= m_canvasMin.y && mouse.x < m_canvasMax.x && mouse.y < m_canvasMax.y)
    {
        m_menuPin = m_linkFrom;
        m_menuPosition = ToGraph(mouse);
        m_menuRequest = MenuKind::LinkDrop;
    }
}

void MaterialGraphCanvas::DrawGrid(ImDrawList& drawList, const MaterialGraphView& view) const
{
    drawList.AddRectFilled(m_canvasMin, m_canvasMax, kCanvasBackground);
    // Lines every 16 graph units while they are at least 6 pixels apart, and every 128 strong.
    const auto lines = [&](float spacing, ImU32 colour)
    {
        const float step = spacing * m_scale;
        if (step < 6.0f)
        {
            return;
        }
        const float startX = std::floor(view.origin.x / spacing) * spacing;
        for (float x = startX;; x += spacing)
        {
            const float screenX = m_canvasMin.x + (x - view.origin.x) * m_scale;
            if (screenX > m_canvasMax.x)
            {
                break;
            }
            drawList.AddLine(ImVec2(screenX, m_canvasMin.y), ImVec2(screenX, m_canvasMax.y), colour);
        }
        const float startY = std::floor(view.origin.y / spacing) * spacing;
        for (float y = startY;; y += spacing)
        {
            const float screenY = m_canvasMin.y + (y - view.origin.y) * m_scale;
            if (screenY > m_canvasMax.y)
            {
                break;
            }
            drawList.AddLine(ImVec2(m_canvasMin.x, screenY), ImVec2(m_canvasMax.x, screenY), colour);
        }
    };
    lines(16.0f, kGridMinor);
    lines(128.0f, kGridMajor);
}

void MaterialGraphCanvas::DrawLinks(ImDrawList& drawList, const MaterialShaderGraph& graph, const MaterialGraphSelection& selection) const
{
    const uint32_t hoveredLink = m_hovered && m_mode == Mode::None && NodeAt(ImGui::GetIO().MousePos) == nullptr ? LinkAt(graph, ImGui::GetIO().MousePos) : 0;
    const float thickness = std::max(2.4f * m_scale, 1.0f);
    for (const MaterialShaderLink& link : graph.links)
    {
        const PinLayout* from = FindPin(link.fromNodeId, link.fromSlot, false);
        const PinLayout* to = FindPin(link.toNodeId, link.toSlot, true);
        if (from == nullptr || to == nullptr)
        {
            continue;
        }
        ImVec2 c0;
        ImVec2 c1;
        LinkControls(from->centre, to->centre, m_scale, c0, c1);
        ImU32 colour = GetMaterialGraphPinColor(from->definition->kind);
        float width = thickness;
        if (link.id == selection.link)
        {
            colour = kNodeSelectedBorder;
            width = thickness * 1.6f;
        }
        else if (link.id == hoveredLink)
        {
            colour = Brighter(colour, 0.45f);
            width = thickness * 1.3f;
        }
        drawList.AddBezierCubic(from->centre, c0, c1, to->centre, colour, width);
    }
    if (m_mode == Mode::DraggingLink)
    {
        const PinLayout* from = FindPin(m_linkFrom.nodeId, m_linkFrom.slot, m_linkFrom.input);
        if (from != nullptr)
        {
            ImVec2 end = ImGui::GetIO().MousePos;
            const NodeLayout* owner = nullptr;
            const PinLayout* target = PinAt(end, &owner);
            if (target != nullptr && target->input != m_linkFrom.input && target->definition->kind == m_linkFrom.kind)
            {
                end = target->centre;
            }
            const ImVec2 start = from->centre;
            ImVec2 c0;
            ImVec2 c1;
            if (m_linkFrom.input)
            {
                LinkControls(end, start, m_scale, c0, c1);
                drawList.AddBezierCubic(end, c0, c1, start, GetMaterialGraphPinColor(m_linkFrom.kind), thickness);
            }
            else
            {
                LinkControls(start, end, m_scale, c0, c1);
                drawList.AddBezierCubic(start, c0, c1, end, GetMaterialGraphPinColor(m_linkFrom.kind), thickness);
            }
        }
    }
}

void MaterialGraphCanvas::DrawNodes(ImDrawList& drawList, const MaterialShaderGraph& graph, const MaterialGraphSelection& selection, const Context& context) const
{
    ImFont* font = ImGui::GetFont();
    const float fontSize = m_fontSize * m_view.zoom;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const NodeLayout* hoveredNode = m_hovered && m_mode == Mode::None ? NodeAt(mouse) : nullptr;
    const NodeLayout* pinOwner = nullptr;
    const PinLayout* hoveredPin = m_hovered ? PinAt(mouse, &pinOwner) : nullptr;
    const float rounding = kRounding * m_scale;
    for (const NodeLayout& layout : m_layouts)
    {
        // Off the canvas: skipped.
        if (layout.max.x < m_canvasMin.x || layout.max.y < m_canvasMin.y || layout.min.x > m_canvasMax.x || layout.min.y > m_canvasMax.y)
        {
            continue;
        }
        const MaterialShaderNode* node = FindMaterialGraphNode(graph, layout.nodeId);
        if (node == nullptr)
        {
            continue;
        }
        const NodeMetrics metrics = MeasureNode(*node, m_scale, fontSize);
        const bool selected = selection.nodes.contains(layout.nodeId);
        // A soft shadow, the body, the coloured title bar.
        const ImVec2 shadow(3.0f * m_scale, 4.0f * m_scale);
        drawList.AddRectFilled(layout.min + shadow, layout.max + shadow, IM_COL32(0, 0, 0, 90), rounding);
        drawList.AddRectFilled(layout.min, layout.max, kNodeBody, rounding);
        const ImU32 header = GetMaterialGraphHeaderColor(node->type);
        drawList.AddRectFilledMultiColor(
            layout.min + ImVec2(rounding * 0.3f, 0.0f),
            ImVec2(layout.max.x - rounding * 0.3f, layout.min.y + layout.headerHeight),
            Brighter(header, 0.12f),
            header,
            header,
            Brighter(header, 0.12f));
        drawList.AddRectFilled(layout.min, ImVec2(layout.max.x, layout.min.y + layout.headerHeight), header, rounding, ImDrawFlags_RoundCornersTop);
        const std::string title = MaterialGraphNodeTitle(*node);
        if (fontSize >= 4.0f)
        {
            drawList.PushClipRect(layout.min, ImVec2(layout.max.x - metrics.padding * 0.5f, layout.max.y), true);
            drawList.AddText(
                font, fontSize, ImVec2(layout.min.x + metrics.padding, layout.min.y + (layout.headerHeight - fontSize) * 0.5f), kNodeText, title.c_str());
            drawList.PopClipRect();
        }

        // The type's own part of the body.
        const float bodyTop = layout.min.y + layout.headerHeight + metrics.padding * 0.5f;
        if (node->type == MaterialShaderNodeType::Texture)
        {
            const ImVec2 thumbMin(layout.min.x + metrics.padding, bodyTop + metrics.padding * 0.25f);
            const ImVec2 thumbMax = thumbMin + ImVec2(metrics.extra, metrics.extra);
            const ImTextureRef thumbnail = context.thumbnail && !node->texturePath.empty() ? context.thumbnail(node->texturePath) : ImTextureRef();
            if (thumbnail.GetTexID() != ImTextureID_Invalid || thumbnail._TexData != nullptr)
            {
                drawList.AddImage(thumbnail, thumbMin, thumbMax);
            }
            else
            {
                // A checkerboard where there is no image (yet).
                const float cell = metrics.extra / 4.0f;
                for (int y = 0; y < 4; ++y)
                {
                    for (int x = 0; x < 4; ++x)
                    {
                        drawList.AddRectFilled(
                            thumbMin + ImVec2(x * cell, y * cell),
                            thumbMin + ImVec2((x + 1) * cell, (y + 1) * cell),
                            (x + y) % 2 == 0 ? IM_COL32(70, 70, 74, 255) : IM_COL32(48, 48, 52, 255));
                    }
                }
            }
            drawList.AddRect(thumbMin, thumbMax, IM_COL32(0, 0, 0, 200));
            if (fontSize >= 4.0f)
            {
                const std::string file = node->texturePath.empty() ? std::string("No texture") : std::filesystem::path(node->texturePath).filename().string();
                drawList.PushClipRect(layout.min, ImVec2(layout.max.x - metrics.padding * 0.5f, layout.max.y), true);
                drawList.AddText(font, fontSize * 0.9f, ImVec2(layout.min.x + metrics.padding, thumbMax.y + metrics.padding * 0.5f), kNodeTextDim, file.c_str());
                drawList.PopClipRect();
            }
        }
        else if (node->type == MaterialShaderNodeType::Color)
        {
            const ImVec2 swatchMin(layout.min.x + metrics.padding, bodyTop + metrics.rowHeight * 0.15f);
            const ImVec2 swatchMax = swatchMin + ImVec2(metrics.extra, metrics.rowHeight * 0.7f);
            const ImVec4 colour(
                std::pow(std::clamp(node->colorValue[0], 0.0f, 1.0f), 1.0f / 2.2f),
                std::pow(std::clamp(node->colorValue[1], 0.0f, 1.0f), 1.0f / 2.2f),
                std::pow(std::clamp(node->colorValue[2], 0.0f, 1.0f), 1.0f / 2.2f),
                1.0f);
            drawList.AddRectFilled(swatchMin, swatchMax, ImGui::ColorConvertFloat4ToU32(colour), 2.0f * m_scale);
            drawList.AddRect(swatchMin, swatchMax, IM_COL32(0, 0, 0, 200), 2.0f * m_scale);
        }

        // The pins and their labels.
        for (const PinLayout& pin : layout.pins)
        {
            const ImU32 colour = GetMaterialGraphPinColor(pin.definition->kind);
            const bool pinHovered = &pin == hoveredPin;
            bool fits = false;
            if (m_mode == Mode::DraggingLink)
            {
                fits = pin.input != m_linkFrom.input && pin.definition->kind == m_linkFrom.kind && layout.nodeId != m_linkFrom.nodeId;
            }
            const float radius = metrics.pinRadius * (pinHovered || fits ? 1.3f : 1.0f);
            if (pin.linked)
            {
                drawList.AddCircleFilled(pin.centre, radius, colour, 16);
            }
            else
            {
                drawList.AddCircleFilled(pin.centre, radius, IM_COL32(20, 20, 22, 255), 16);
                drawList.AddCircle(pin.centre, radius, colour, 16, std::max(1.6f * m_scale, 1.0f));
            }
            if (m_mode == Mode::DraggingLink && !fits)
            {
                // What the link cannot reach is dimmed.
                drawList.AddCircleFilled(pin.centre, radius, IM_COL32(20, 20, 22, 150), 16);
            }
            if (fontSize < 4.0f)
            {
                continue;
            }
            const std::string text = PinText(*node, *pin.definition, pin.input);
            const float textY = pin.centre.y - fontSize * 0.5f;
            const ImU32 textColour = pinHovered ? IM_COL32(255, 255, 255, 255) : (pin.linked || !pin.input ? kNodeText : kNodeTextDim);
            if (pin.input)
            {
                const float x = pin.centre.x + metrics.pinRadius + 6.0f * m_scale;
                drawList.AddText(font, fontSize, ImVec2(x, textY), textColour, text.c_str());
                // The Output's own factor beside an input nothing feeds; the Blend's default factor.
                std::string value;
                if (!pin.linked && node->type == MaterialShaderNodeType::Output && pin.definition->kind == MaterialGraphPinKind::Scalar)
                {
                    value = OutputFactorText(*node, pin.definition->slot);
                }
                else if (!pin.linked && node->type == MaterialShaderNodeType::Blend && std::string_view(pin.definition->slot) == "factor")
                {
                    char buffer[32] = {};
                    std::snprintf(buffer, sizeof(buffer), "%.2f", node->scalarValue);
                    value = buffer;
                }
                if (!value.empty())
                {
                    const float labelWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str()).x;
                    drawList.AddText(font, fontSize, ImVec2(x + labelWidth + 8.0f * m_scale, textY), IM_COL32(120, 120, 128, 255), value.c_str());
                }
                if (!pin.linked && node->type == MaterialShaderNodeType::Output && pin.definition->kind == MaterialGraphPinKind::Color)
                {
                    const float* factor = std::string_view(pin.definition->slot) == "base_factor" ? node->pbr.baseColorFactor : node->pbr.emissiveColor;
                    const float labelWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str()).x;
                    const ImVec2 swatchMin(x + labelWidth + 8.0f * m_scale, pin.centre.y - fontSize * 0.35f);
                    const ImVec2 swatchMax = swatchMin + ImVec2(fontSize * 1.6f, fontSize * 0.7f);
                    const ImVec4 colour(
                        std::pow(std::clamp(factor[0], 0.0f, 1.0f), 1.0f / 2.2f),
                        std::pow(std::clamp(factor[1], 0.0f, 1.0f), 1.0f / 2.2f),
                        std::pow(std::clamp(factor[2], 0.0f, 1.0f), 1.0f / 2.2f),
                        1.0f);
                    drawList.AddRectFilled(swatchMin, swatchMax, ImGui::ColorConvertFloat4ToU32(colour), 2.0f * m_scale);
                }
            }
            else
            {
                const float width = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str()).x;
                drawList.AddText(font, fontSize, ImVec2(pin.centre.x - metrics.pinRadius - 6.0f * m_scale - width, textY), textColour, text.c_str());
            }
        }

        const bool hovered = hoveredNode == &layout;
        const ImU32 border = selected ? kNodeSelectedBorder : (hovered ? kNodeHoverBorder : kNodeBorder);
        drawList.AddRect(layout.min, layout.max, border, rounding, 0, selected ? std::max(2.2f * m_scale, 1.5f) : std::max(1.0f * m_scale, 1.0f));
    }
}

void MaterialGraphCanvas::DrawOverlay(ImDrawList& drawList, const MaterialGraphView& view) const
{
    ImFont* font = ImGui::GetFont();
    const float uiScale = std::max(ImGui::GetStyle().FontScaleMain, 0.1f);
    // Unreal's watermark, bottom right.
    const float markSize = 34.0f * uiScale;
    const char* mark = "MATERIAL";
    const ImVec2 markExtent = font->CalcTextSizeA(markSize, FLT_MAX, 0.0f, mark);
    drawList.AddText(font, markSize, ImVec2(m_canvasMax.x - markExtent.x - 16.0f * uiScale, m_canvasMax.y - markExtent.y - 10.0f * uiScale), IM_COL32(255, 255, 255, 20), mark);
    // The zoom, top right.
    char zoom[32] = {};
    std::snprintf(zoom, sizeof(zoom), "Zoom %d%%", static_cast<int>(std::round(view.zoom * 100.0f)));
    const ImVec2 zoomExtent = ImGui::CalcTextSize(zoom);
    drawList.AddText(ImVec2(m_canvasMax.x - zoomExtent.x - 10.0f * uiScale, m_canvasMin.y + 8.0f * uiScale), IM_COL32(200, 200, 205, 140), zoom);
    if (m_mode == Mode::BoxSelecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 boxMin(std::min(m_pressPosition.x, mouse.x), std::min(m_pressPosition.y, mouse.y));
        const ImVec2 boxMax(std::max(m_pressPosition.x, mouse.x), std::max(m_pressPosition.y, mouse.y));
        drawList.AddRectFilled(boxMin, boxMax, kBoxFill);
        drawList.AddRect(boxMin, boxMax, kBoxBorder);
    }
}

bool MaterialGraphCanvas::DrawAddNodeList(MaterialShaderGraph& graph, MaterialGraphSelection& selection, const std::optional<PinRef>& fitting)
{
    if (m_menuFocusSearch)
    {
        ImGui::SetKeyboardFocusHere();
        m_menuFocusSearch = false;
    }
    ImGui::SetNextItemWidth(220.0f * std::max(ImGui::GetStyle().FontScaleMain, 0.1f));
    const bool entered = ImGui::InputTextWithHint("##search", "Search nodes", m_menuSearch, sizeof(m_menuSearch), ImGuiInputTextFlags_EnterReturnsTrue);
    // Which nodes fit a dropped link: one with a pin of the link's kind the other way round.
    const auto fits = [&](const MaterialGraphNodeKind& kind)
    {
        if (!fitting.has_value())
        {
            return true;
        }
        const auto& pins = fitting->input ? GetMaterialGraphOutputPins(kind.type) : GetMaterialGraphInputPins(kind.type);
        return std::any_of(pins.begin(), pins.end(), [&](const MaterialGraphPinDefinition& pin)
                           {
                               return pin.kind == fitting->kind;
                           });
    };
    const MaterialGraphNodeKind* chosen = nullptr;
    const MaterialGraphNodeKind* first = nullptr;
    const char* category = nullptr;
    for (const MaterialGraphNodeKind& kind : MaterialGraphNodeKinds())
    {
        if (!fits(kind) || !FuzzyMatchScore(m_menuSearch, kind.name).has_value())
        {
            continue;
        }
        const bool unavailable = kind.type == MaterialShaderNodeType::Output && MaterialGraphHasOutputNode(graph);
        if (category == nullptr || std::string_view(category) != kind.category)
        {
            category = kind.category;
            ImGui::SeparatorText(category);
        }
        ImGui::BeginDisabled(unavailable);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Brighter(GetMaterialGraphHeaderColor(kind.type), 0.55f)));
        if (ImGui::Selectable(kind.name))
        {
            chosen = &kind;
        }
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", kind.description);
        if (first == nullptr && !unavailable)
        {
            first = &kind;
        }
    }
    if (entered && chosen == nullptr)
    {
        chosen = first;
    }
    if (chosen == nullptr)
    {
        return false;
    }
    MaterialShaderNode* added = AddMaterialGraphNode(graph, chosen->type, m_menuPosition);
    const uint32_t addedId = added->id;
    const MaterialShaderNodeType addedType = added->type;
    selection.nodes = {addedId};
    selection.link = 0;
    if (fitting.has_value())
    {
        // Connected to the dropped link through its first pin that fits; a node fed from the left
        // goes to the right of the drop, one feeding the left to the left.
        const auto& pins = fitting->input ? GetMaterialGraphOutputPins(addedType) : GetMaterialGraphInputPins(addedType);
        for (const MaterialGraphPinDefinition& pin : pins)
        {
            if (pin.kind != fitting->kind)
            {
                continue;
            }
            const uint32_t link = fitting->input ? ConnectMaterialGraphPins(graph, addedId, pin.slot, fitting->nodeId, fitting->slot)
                                                 : ConnectMaterialGraphPins(graph, fitting->nodeId, fitting->slot, addedId, pin.slot);
            if (link != 0)
            {
                break;
            }
        }
        if (fitting->input)
        {
            if (MaterialShaderNode* node = FindMaterialGraphNode(graph, addedId))
            {
                node->position.x -= MaterialGraphNodeSize(*node).x;
            }
        }
    }
    ImGui::CloseCurrentPopup();
    return true;
}

void MaterialGraphCanvas::DrawMenus(MaterialShaderGraph& graph, MaterialGraphSelection& selection, Result& result)
{
    constexpr const char* kMenu = "##material_graph_menu";
    if (m_menuRequest.has_value())
    {
        m_menuKind = *m_menuRequest;
        m_menuRequest.reset();
        m_menuSearch[0] = '\0';
        m_menuFocusSearch = true;
        ImGui::OpenPopup(kMenu);
    }
    if (!ImGui::BeginPopup(kMenu))
    {
        return;
    }
    switch (m_menuKind)
    {
    case MenuKind::Background:
    {
        ImGui::TextDisabled("Add Node");
        result.changed |= DrawAddNodeList(graph, selection, std::nullopt);
        ImGui::Separator();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, HasClipboard()))
        {
            std::vector<uint32_t> pasted = PasteMaterialGraphNodes(graph, m_clipboard, m_menuPosition);
            selection.nodes = std::set<uint32_t>(pasted.begin(), pasted.end());
            selection.link = 0;
            result.changed = !pasted.empty();
        }
        if (ImGui::MenuItem("Select All", "Ctrl+A"))
        {
            SelectAll(graph, selection);
        }
        if (ImGui::MenuItem("Frame All", "Home"))
        {
            m_frameRequest = std::set<uint32_t>{};
        }
        if (ImGui::MenuItem("Arrange Nodes"))
        {
            ArrangeMaterialGraph(graph);
            m_frameRequest = std::set<uint32_t>{};
            result.changed = true;
        }
        break;
    }
    case MenuKind::Node:
    {
        const MaterialShaderNode* node = FindMaterialGraphNode(graph, m_menuNode);
        if (node == nullptr)
        {
            ImGui::CloseCurrentPopup();
            break;
        }
        ImGui::TextDisabled("%s", MaterialGraphNodeTitle(*node).c_str());
        ImGui::Separator();
        if (node->type == MaterialShaderNodeType::Texture && ImGui::MenuItem("Choose Texture..."))
        {
            result.pickTextureForNode = node->id;
        }
        const bool onlyOutput = selection.nodes.size() == 1 && node->type == MaterialShaderNodeType::Output;
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, !onlyOutput))
        {
            CopySelection(graph, selection);
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, !onlyOutput))
        {
            CopySelection(graph, selection);
            result.changed |= DeleteSelection(graph, selection);
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !onlyOutput))
        {
            result.changed |= DuplicateSelection(graph, selection);
        }
        if (ImGui::MenuItem("Delete", "Delete", false, !onlyOutput))
        {
            result.changed |= DeleteSelection(graph, selection);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Break All Links"))
        {
            const size_t before = graph.links.size();
            std::erase_if(graph.links, [&](const MaterialShaderLink& link)
                          {
                              return selection.nodes.contains(link.fromNodeId) || selection.nodes.contains(link.toNodeId);
                          });
            result.changed |= graph.links.size() != before;
        }
        break;
    }
    case MenuKind::Pin:
    {
        if (!m_menuPin.has_value())
        {
            ImGui::CloseCurrentPopup();
            break;
        }
        const MaterialShaderNode* node = FindMaterialGraphNode(graph, m_menuPin->nodeId);
        const MaterialGraphPinDefinition* pin = node != nullptr ? FindMaterialGraphPin(node->type, m_menuPin->slot, m_menuPin->input) : nullptr;
        ImGui::TextDisabled("%s", pin != nullptr ? pin->label : "Pin");
        ImGui::Separator();
        if (ImGui::MenuItem("Break Link(s)", "Alt+Click"))
        {
            result.changed |= BreakMaterialGraphPinLinks(graph, m_menuPin->nodeId, m_menuPin->slot, m_menuPin->input);
        }
        break;
    }
    case MenuKind::Link:
    {
        if (ImGui::MenuItem("Delete Link", "Alt+Click"))
        {
            RemoveMaterialGraphLink(graph, m_menuLink);
            selection.link = 0;
            result.changed = true;
        }
        break;
    }
    case MenuKind::LinkDrop:
    {
        ImGui::TextDisabled("Add a node that fits");
        result.changed |= DrawAddNodeList(graph, selection, m_menuPin);
        break;
    }
    }
    ImGui::EndPopup();
}

bool MaterialGraphCanvas::DeleteSelection(MaterialShaderGraph& graph, MaterialGraphSelection& selection)
{
    bool changed = false;
    for (const uint32_t id : selection.nodes)
    {
        const MaterialShaderNode* node = FindMaterialGraphNode(graph, id);
        if (node != nullptr && node->type != MaterialShaderNodeType::Output)
        {
            RemoveMaterialGraphNode(graph, id);
            changed = true;
        }
    }
    if (selection.link != 0 && FindMaterialGraphLink(graph, selection.link) != nullptr)
    {
        RemoveMaterialGraphLink(graph, selection.link);
        changed = true;
    }
    selection.link = 0;
    std::erase_if(selection.nodes, [&](uint32_t id)
                  {
                      return FindMaterialGraphNode(graph, id) == nullptr;
                  });
    return changed;
}

void MaterialGraphCanvas::CopySelection(const MaterialShaderGraph& graph, const MaterialGraphSelection& selection)
{
    MaterialGraphClipboard copied = CopyMaterialGraphNodes(graph, selection.nodes);
    if (!copied.IsEmpty())
    {
        m_clipboard = std::move(copied);
    }
}

bool MaterialGraphCanvas::Paste(MaterialShaderGraph& graph, MaterialGraphSelection& selection, const MaterialGraphView& view)
{
    if (m_clipboard.IsEmpty())
    {
        return false;
    }
    MaterialGraphNodePosition at;
    if (m_hovered)
    {
        at = ToGraph(ImGui::GetIO().MousePos);
    }
    else
    {
        const float uiScale = std::max(ImGui::GetStyle().FontScaleMain, 0.1f);
        at.x = view.origin.x + (m_canvasMax.x - m_canvasMin.x) * 0.4f / (uiScale * view.zoom);
        at.y = view.origin.y + (m_canvasMax.y - m_canvasMin.y) * 0.4f / (uiScale * view.zoom);
    }
    const std::vector<uint32_t> pasted = PasteMaterialGraphNodes(graph, m_clipboard, at);
    selection.nodes = std::set<uint32_t>(pasted.begin(), pasted.end());
    selection.link = 0;
    return !pasted.empty();
}

bool MaterialGraphCanvas::DuplicateSelection(MaterialShaderGraph& graph, MaterialGraphSelection& selection)
{
    const MaterialGraphClipboard copied = CopyMaterialGraphNodes(graph, selection.nodes);
    if (copied.IsEmpty())
    {
        return false;
    }
    MaterialGraphNodePosition topLeft{FLT_MAX, FLT_MAX};
    for (const MaterialShaderNode& node : copied.nodes)
    {
        topLeft.x = std::min(topLeft.x, node.position.x);
        topLeft.y = std::min(topLeft.y, node.position.y);
    }
    const std::vector<uint32_t> pasted = PasteMaterialGraphNodes(graph, copied, MaterialGraphNodePosition{topLeft.x + 32.0f, topLeft.y + 32.0f});
    selection.nodes = std::set<uint32_t>(pasted.begin(), pasted.end());
    selection.link = 0;
    return true;
}

void MaterialGraphCanvas::SelectAll(const MaterialShaderGraph& graph, MaterialGraphSelection& selection)
{
    selection.nodes.clear();
    for (const MaterialShaderNode& node : graph.nodes)
    {
        selection.nodes.insert(node.id);
    }
    selection.link = 0;
}

void MaterialGraphCanvas::FrameSelection(const MaterialGraphSelection& selection)
{
    m_frameRequest = selection.nodes;
}

std::optional<ImVec2> MaterialGraphCanvas::NodeTitleOnScreen(uint32_t nodeId) const
{
    for (const NodeLayout& layout : m_layouts)
    {
        if (layout.nodeId == nodeId)
        {
            return ImVec2(0.5f * (layout.min.x + layout.max.x), layout.min.y + 0.5f * layout.headerHeight);
        }
    }
    return std::nullopt;
}

void MaterialGraphCanvas::CancelInteraction()
{
    m_mode = Mode::None;
    m_menuRequest.reset();
    m_dragStart.clear();
    m_clickSelectsOnly = 0;
}
}
