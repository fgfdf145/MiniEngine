#pragma once

// The editor's commands: what the main menu, the toolbar and the keyboard shortcuts run.

#include "command_registry.h"
#include "ui/editor_menu_toolbar.h"

#include <engine/renderer/render_types.h>
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

enum class AntiAliasingMode
{
    Taa,
    None
};

// What the checkable commands read and write. The editor UI copies the transform tool, the debug
// view, tone mapping, anti-aliasing and the play state to and from the scene, the renderer and the
// driven car each frame, and the viewport reads the gizmo switch. The pipeline, ray tracing and
// wireframe commands stay disabled while the renderer has nothing behind them.
struct EditorCommandState
{
    TransformTool transformTool = TransformTool::Move;
    PlayState playState = PlayState::Stopped;
    RenderPipelineMode pipelineMode = RenderPipelineMode::Rasterization;
    // View: what the viewport shows, for debugging; every view the tone mapping pass has.
    GBufferDebugView debugView = GBufferDebugView::Off;
    ToneMapper toneMapping = ToneMapper::Gt7;
    AntiAliasingMode antiAliasing = AntiAliasingMode::Taa;
    bool rayTracing = false;
    bool wireframe = false;
    bool gizmos = true;
    // The viewport fills the whole screen, borderless, with every panel, menu and toolbar hidden.
    // The editor UI turns the window fullscreen and back.
    bool viewportFullscreen = false;
    // The renderer traces no hardware rays and has no GPU path tracer (DDGI traces its own compute
    // BVH), so Hybrid, Path Tracing and the Ray Tracing switch are disabled until it does.
    bool rayTracingSupported = false;
    // The material pipelines are fill-only; Wireframe is disabled until they have line variants.
    bool wireframeSupported = false;
    // The G-buffer exists: the forward-only order writes none, so only the shaded view is offered.
    bool gbufferAvailable = true;
    // The Khronos reference view picks its own tone mapping, so the choice is disabled while it is on.
    bool khronosReference = false;
    // The command palette (a search over every command's label) opens when this is set.
    bool commandPaletteRequested = false;
    // The viewport is being recorded to a video (Tools > Record Viewport). The editor UI sets it
    // from the backend every frame.
    bool videoRecording = false;
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
// function is empty is disabled: the menus show it greyed out and its shortcut stays free.
struct EditorSceneCommands
{
    std::function<void()> newScene;
    std::function<void()> openScene;
    std::function<void()> saveScene;
    std::function<void()> saveSceneAs;
    std::function<void()> importModel;
    std::function<void()> exit;
    std::function<void()> undo;
    std::function<void()> redo;
    std::function<void()> cut;
    std::function<void()> copy;
    std::function<void()> paste;
    std::function<void()> duplicate;
    std::function<void()> deleteSelection;
    std::function<bool()> hasSelection; // enables Delete; empty means always enabled
    std::function<void()> openPreferences;
    std::function<void()> clearScene;
    std::function<void()> createEntity;
    std::function<void(LightType)> createLight;
    std::function<void()> openSceneSettings;
    std::function<void()> captureViewport;
    std::function<void()> toggleVideoRecording; // Record Viewport: starts or stops
    std::function<void()> stepSimulation; // Step: one fixed physics step while paused
    std::function<void()> reloadShaders;
    std::function<void()> showShaderLog;
    std::function<void()> clearShaderCache;
    std::function<void()> showDocumentation;
    std::function<void()> showKeyboardShortcuts;
    std::function<void()> showAbout;
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

// The View command that shows this debug view, e.g. "view.debug.albedo".
std::string DebugViewCommandId(GBufferDebugView view);
}
