#pragma once

// The Material Editor's node graph (docs/design/2026-10-09-material-editor-redesign-design.md): what a
// material shader graph's nodes and pins are, the edits made on it, and the canvas that draws it and
// takes the mouse as Unreal's material graph does.

#include <engine/scene/scene_components.h>

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

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

const std::vector<MaterialGraphPinDefinition>& GetMaterialGraphInputPins(MaterialShaderNodeType type);
const std::vector<MaterialGraphPinDefinition>& GetMaterialGraphOutputPins(MaterialShaderNodeType type);
const MaterialGraphPinDefinition* FindMaterialGraphPin(MaterialShaderNodeType type, std::string_view slot, bool input);

// What the palette and the add-node menu offer: each node type with its name, category and what it
// is for.
struct MaterialGraphNodeKind
{
    MaterialShaderNodeType type = MaterialShaderNodeType::Texture;
    const char* name = "";
    const char* category = "";
    const char* description = "";
};
std::span<const MaterialGraphNodeKind> MaterialGraphNodeKinds();

const char* GetMaterialGraphNodeTypeLabel(MaterialShaderNodeType type);
// The node's name, or its type's default when it has none.
std::string MaterialGraphNodeTitle(const MaterialShaderNode& node);
ImU32 GetMaterialGraphPinColor(MaterialGraphPinKind kind);
ImU32 GetMaterialGraphHeaderColor(MaterialShaderNodeType type);

const MaterialShaderNode* FindMaterialGraphNode(const MaterialShaderGraph& graph, uint32_t nodeId);
MaterialShaderNode* FindMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId);
const MaterialShaderLink* FindMaterialGraphLink(const MaterialShaderGraph& graph, uint32_t linkId);
const MaterialShaderLink* FindIncomingMaterialGraphLink(const MaterialShaderGraph& graph, uint32_t nodeId, std::string_view slot);
MaterialShaderNode* FindMaterialGraphOutputNode(MaterialShaderGraph& graph);
bool MaterialGraphHasOutputNode(const MaterialShaderGraph& graph);

MaterialShaderNode* AddMaterialGraphNode(MaterialShaderGraph& graph, MaterialShaderNodeType type, const MaterialGraphNodePosition& position);
void RemoveMaterialGraphLink(MaterialShaderGraph& graph, uint32_t linkId);
// Removes the node and every link into or out of it.
void RemoveMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId);
// Removes every link at one of a node's pins. Returns whether there were any.
bool BreakMaterialGraphPinLinks(MaterialShaderGraph& graph, uint32_t nodeId, std::string_view slot, bool input);
// Whether an output pin may feed an input pin: both exist, the kinds match, they are on different
// nodes and the link closes no cycle. failureReason says why not.
bool CanConnectMaterialGraphPins(
    const MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    std::string_view fromSlot,
    uint32_t toNodeId,
    std::string_view toSlot,
    std::string* failureReason = nullptr);
// Links the pins (an input takes one link: the one it had goes). Returns the link, 0 when it cannot.
uint32_t ConnectMaterialGraphPins(
    MaterialShaderGraph& graph, uint32_t fromNodeId, std::string_view fromSlot, uint32_t toNodeId, std::string_view toSlot);

// Copied nodes and the links among them, positions as they were.
struct MaterialGraphClipboard
{
    std::vector<MaterialShaderNode> nodes;
    std::vector<MaterialShaderLink> links;

    bool IsEmpty() const
    {
        return nodes.empty();
    }
};
// The selected nodes, but the Output node (a graph has one, so it is never copied).
MaterialGraphClipboard CopyMaterialGraphNodes(const MaterialShaderGraph& graph, const std::set<uint32_t>& nodeIds);
// Adds copies with new ids, laid out as they were with their top-left node at `at`, and their links.
// Returns the new nodes' ids.
std::vector<uint32_t> PasteMaterialGraphNodes(MaterialShaderGraph& graph, const MaterialGraphClipboard& clipboard, const MaterialGraphNodePosition& at);

// Drops links to nodes that are not there (graphs saved while the default graph's builder kept a
// pointer across a reallocation have an Output link to no node), and, when the Output's surface input
// is then empty and exactly one Surface or Blend node feeds nothing, links it there, as the graph was
// meant to be. Returns whether anything changed.
bool RepairMaterialGraphLinks(MaterialShaderGraph& graph);

// A node's size in graph units (points at zoom 1 and UI scale 1), as the canvas draws it.
ImVec2 MaterialGraphNodeSize(const MaterialShaderNode& node);
// Lays the graph out in columns from the Output node leftward, each node one column left of the
// nodes it feeds, ordered down the column as the pins it feeds are; nodes that feed nothing go in a
// column of their own at the left.
void ArrangeMaterialGraph(MaterialShaderGraph& graph);

// Where the canvas looks: the graph point at its top-left corner, and the zoom.
struct MaterialGraphView
{
    MaterialGraphNodePosition origin{};
    float zoom = 1.0f;
    // Frames the whole graph the next time it is drawn (a graph shown for the first time).
    bool frameAll = true;
};

struct MaterialGraphSelection
{
    std::set<uint32_t> nodes;
    // A selected link; 0 for none.
    uint32_t link = 0;

    bool IsEmpty() const
    {
        return nodes.empty() && link == 0;
    }
};

inline constexpr float kMaterialGraphMinZoom = 0.1f;
inline constexpr float kMaterialGraphMaxZoom = 2.0f;

// The graph canvas: draws a graph over the whole of the current window's content region and takes
// its mouse (Unreal's graph editor):
//   left click a node        select it (Ctrl toggles, Shift adds); drag to move the selection
//   left drag the background box selection (Ctrl removes, Shift adds)
//   left drag from a pin     a link; dropped on a pin it connects, on a node its first fitting pin,
//                            on the background a menu of the nodes that fit, added connected
//   Ctrl + drag a linked input   takes that link to another input
//   Alt + click a pin or link    breaks it
//   right or middle drag     pan;  wheel: zoom about the cursor
//   right click              the menu for what is under the cursor (add node, node, pin, link)
// The window owns the keyboard and calls the edit functions below for its shortcuts.
class MaterialGraphCanvas
{
  public:
    struct Context
    {
        float uiScale = 1.0f;
        // A small image of a texture file for Texture nodes; empty while it loads, or for none.
        std::function<ImTextureRef(const std::string& path)> thumbnail;
        std::string* statusMessage = nullptr;
    };
    struct Result
    {
        bool changed = false;
        // A drag that changes the graph as it goes is under way (moving nodes): the window keeps it
        // one undo step.
        bool dragging = false;
        // A Texture node to pick a file for (double-clicked, or its menu's Choose Texture).
        std::optional<uint32_t> pickTextureForNode;
    };

    Result Draw(MaterialShaderGraph& graph, MaterialGraphView& view, MaterialGraphSelection& selection, const Context& context);

    // The edits the window's shortcuts and toolbar make. Each returns whether the graph changed.
    bool DeleteSelection(MaterialShaderGraph& graph, MaterialGraphSelection& selection);
    void CopySelection(const MaterialShaderGraph& graph, const MaterialGraphSelection& selection);
    // At the cursor when it is over the canvas, else in the middle of the view.
    bool Paste(MaterialShaderGraph& graph, MaterialGraphSelection& selection, const MaterialGraphView& view);
    bool DuplicateSelection(MaterialShaderGraph& graph, MaterialGraphSelection& selection);
    void SelectAll(const MaterialShaderGraph& graph, MaterialGraphSelection& selection);
    bool HasClipboard() const
    {
        return !m_clipboard.IsEmpty();
    }
    // Frames the selected nodes, or every node with none selected, at the next draw.
    void FrameSelection(const MaterialGraphSelection& selection);
    // Whether the mouse was over the canvas in the last frame it was drawn.
    bool IsHovered() const
    {
        return m_hovered;
    }
    // Drops every drag and menu in progress (a different graph is shown).
    void CancelInteraction();
    // Where a node's title bar is on screen as last drawn (tests drive the mouse by it).
    std::optional<ImVec2> NodeTitleOnScreen(uint32_t nodeId) const;
    ImVec2 CanvasMin() const
    {
        return m_canvasMin;
    }
    ImVec2 CanvasMax() const
    {
        return m_canvasMax;
    }

  private:
    struct PinLayout
    {
        const MaterialGraphPinDefinition* definition = nullptr;
        ImVec2 centre{0.0f, 0.0f};
        // The pin and its label, where a press grabs the pin.
        ImVec2 hitMin{0.0f, 0.0f};
        ImVec2 hitMax{0.0f, 0.0f};
        bool input = true;
        bool linked = false;
    };
    struct NodeLayout
    {
        uint32_t nodeId = 0;
        ImVec2 min{0.0f, 0.0f};
        ImVec2 max{0.0f, 0.0f};
        float headerHeight = 0.0f;
        std::vector<PinLayout> pins;
    };
    struct PinRef
    {
        uint32_t nodeId = 0;
        std::string slot;
        bool input = true;
        MaterialGraphPinKind kind = MaterialGraphPinKind::Texture;
    };
    enum class Mode
    {
        None,
        // The right button is down: a drag pans, a click opens the menu.
        RightPressed,
        Panning,
        DraggingNodes,
        BoxSelecting,
        DraggingLink
    };
    enum class MenuKind
    {
        Background,
        Node,
        Pin,
        Link,
        // A link dropped on the background: add a node it fits, connected.
        LinkDrop
    };

    // Where every node and pin is on screen this frame.
    void Layout(const MaterialShaderGraph& graph, const ImVec2& canvasMin, const MaterialGraphView& view, float scale, float fontSize);
    ImVec2 ToScreen(const MaterialGraphNodePosition& position) const;
    MaterialGraphNodePosition ToGraph(const ImVec2& screen) const;
    const NodeLayout* NodeAt(const ImVec2& point) const;
    const PinLayout* PinAt(const ImVec2& point, const NodeLayout** owner) const;
    uint32_t LinkAt(const MaterialShaderGraph& graph, const ImVec2& point) const;
    const PinLayout* FindPin(uint32_t nodeId, std::string_view slot, bool input) const;

    void HandleMouse(MaterialShaderGraph& graph, MaterialGraphView& view, MaterialGraphSelection& selection, bool hovered, Result& result);
    void FinishLinkDrag(MaterialShaderGraph& graph, MaterialGraphSelection& selection, Result& result);
    void DrawGrid(ImDrawList& drawList, const MaterialGraphView& view) const;
    void DrawLinks(ImDrawList& drawList, const MaterialShaderGraph& graph, const MaterialGraphSelection& selection) const;
    void DrawNodes(ImDrawList& drawList, const MaterialShaderGraph& graph, const MaterialGraphSelection& selection, const Context& context) const;
    void DrawOverlay(ImDrawList& drawList, const MaterialGraphView& view) const;
    void DrawMenus(MaterialShaderGraph& graph, MaterialGraphSelection& selection, Result& result);
    // The add-node list under a search box; with a link drop, only the nodes that fit it. Returns
    // whether one was added.
    bool DrawAddNodeList(MaterialShaderGraph& graph, MaterialGraphSelection& selection, const std::optional<PinRef>& fitting);
    void ApplyFraming(const MaterialShaderGraph& graph, MaterialGraphView& view);

    // This frame's geometry.
    ImVec2 m_canvasMin{0.0f, 0.0f};
    ImVec2 m_canvasMax{0.0f, 0.0f};
    float m_scale = 1.0f;
    float m_fontSize = 13.0f;
    MaterialGraphView m_view;
    std::vector<NodeLayout> m_layouts;
    bool m_hovered = false;

    Mode m_mode = Mode::None;
    ImVec2 m_pressPosition{0.0f, 0.0f};
    // Nodes being dragged and where they started; the mouse's graph position at the start.
    std::vector<std::pair<uint32_t, MaterialGraphNodePosition>> m_dragStart;
    MaterialGraphNodePosition m_dragMouseStart{};
    bool m_dragMoved = false;
    // A press on a node already in a selection of several: a click without a drag selects it alone.
    uint32_t m_clickSelectsOnly = 0;
    // The box selection's start and the selection it adds to or takes from.
    std::set<uint32_t> m_boxBase;
    bool m_boxSubtract = false;
    PinRef m_linkFrom;

    std::optional<MenuKind> m_menuRequest;
    MenuKind m_menuKind = MenuKind::Background;
    uint32_t m_menuNode = 0;
    uint32_t m_menuLink = 0;
    std::optional<PinRef> m_menuPin;
    MaterialGraphNodePosition m_menuPosition{};
    char m_menuSearch[64] = {};
    bool m_menuFocusSearch = false;

    // Framing requested for the next draw: the nodes to frame (empty frames all).
    std::optional<std::set<uint32_t>> m_frameRequest;
    MaterialGraphClipboard m_clipboard;
};
}
