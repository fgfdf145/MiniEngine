#pragma once

// The editor's commands: what the main menu, the toolbar and the keyboard shortcuts run.

#include "command_registry.h"
#include "ui/editor_menu_toolbar.h"

#include <functional>
#include <span>
#include <string>

namespace me
{

enum class TransformTool
{
    Move,
    Rotate,
    Scale
};

enum class PlayState
{
    Stopped,
    Playing,
    Paused
};

enum class RenderPipelineMode
{
    Rasterization,
    Hybrid,
    PathTracing
};

// View: what the viewport shows, for debugging. Separate from the Render pipeline settings.
enum class ViewportDebugView
{
    Lit,
    Albedo,
    Normal,
    Roughness,
    Depth,
    Bvh
};

enum class ToneMappingMode
{
    Gt7,
    PbrNeutral,
    None
};

enum class AntiAliasingMode
{
    Taa,
    None
};

// What the checkable commands read and write. Nothing reads it yet: each command that changes it
// has a TODO where it should reach the renderer or the scene.
struct EditorCommandState
{
    TransformTool transformTool = TransformTool::Move;
    PlayState playState = PlayState::Stopped;
    RenderPipelineMode pipelineMode = RenderPipelineMode::Rasterization;
    ViewportDebugView debugView = ViewportDebugView::Lit;
    ToneMappingMode toneMapping = ToneMappingMode::Gt7;
    AntiAliasingMode antiAliasing = AntiAliasingMode::Taa;
    bool rayTracing = false;
    bool wireframe = false;
    bool gizmos = true;
    // TODO: from the device's ray tracing support (VK_KHR_ray_tracing_pipeline); limited on MoltenVK.
    bool rayTracingSupported = true;
    // TODO: the command palette (fuzzy search over every Command label) opens when this is set.
    bool commandPaletteRequested = false;
};

// An editor panel the Window menu shows and hides.
struct EditorPanel
{
    std::string id;         // command id suffix: "window.<id>"
    std::string windowName; // the ImGui window's name
    std::string icon;
    bool* visible = nullptr;
};

struct EditorWindowCommands
{
    std::span<const EditorPanel> panels;
    std::function<void()> resetLayout;
};

// Registers every command of the main menu (File, Edit, Scene, View, Render, Tools, Window, Help)
// and of the toolbar. The Window menu lists `window.panels` in order. `state` and the panels'
// flags must outlive the registry.
void RegisterEditorCommands(CommandRegistry& registry, EditorCommandState& state, const EditorWindowCommands& window);

// Transform tools on the left, play controls in the center, pipeline settings on the right.
ToolbarLayout BuildEditorToolbarLayout();
}
