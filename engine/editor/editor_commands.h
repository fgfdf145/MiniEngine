#pragma once

// The editor's commands: what the main menu, the toolbar and the keyboard shortcuts run.

#include "command_registry.h"
#include "ui/editor_menu_toolbar.h"

#include <engine/scene/scene_components.h>

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

// What the checkable commands read and write. The editor UI copies the transform tool, the debug
// view and anti-aliasing to and from the scene and the renderer each frame; each other field that
// nothing reads yet has a TODO where it should reach the renderer or the scene.
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

// What the commands that reach the scene, the files or the application run. A command whose
// function is empty does nothing, as do the commands with no function here yet.
struct EditorSceneCommands
{
    std::function<void()> openScene;
    std::function<void()> saveScene;
    std::function<void()> saveSceneAs;
    std::function<void()> importModel;
    std::function<void()> exit;
    std::function<void()> deleteSelection;
    std::function<bool()> hasSelection; // enables Delete; empty means always enabled
    std::function<void()> createEntity;
    std::function<void(LightType)> createLight;
    std::function<void()> captureViewport;
};

// Registers every command of the main menu (File, Edit, Scene, View, Render, Tools, Window, Help)
// and of the toolbar. The Window menu lists `window.panels` in order. `state` and the panels'
// flags must outlive the registry.
void RegisterEditorCommands(
    CommandRegistry& registry,
    EditorCommandState& state,
    const EditorWindowCommands& window,
    const EditorSceneCommands& scene = {});

// Transform tools on the left, play controls in the center, pipeline settings on the right.
ToolbarLayout BuildEditorToolbarLayout();
}
