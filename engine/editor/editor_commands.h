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
// view, tone mapping, anti-aliasing, the pipeline, ray tracing and the play state to and from the
// scene, the renderer and the driven car each frame, and the viewport reads the gizmo switch. The
// wireframe command stays disabled while the renderer has nothing behind it.
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
    // While a car is driven, the viewport shows Gran Turismo 7's driving HUD (speed, revs, gear, pedals,
    // assists) along its bottom, and the minimap moves to the top right.
    bool drivingHud = true;
    // The scene's map at a corner of the viewport (the bottom left, the top right under the driving HUD).
    bool minimap = false;
    // Everything the editor draws over the viewport's picture: the help text, the view manipulator, the
    // minimap, the driving HUD and the car's physics overlays. Off leaves the picture clean except for
    // the gizmos and the selection, which View > Gizmos switches, and a recording's indicator.
    bool viewportUi = true;
    // The viewport fills the whole screen, borderless, with every panel, menu and toolbar hidden.
    // The editor UI turns the window fullscreen and back.
    bool viewportFullscreen = false;
    // The pipeline modes map onto the render settings: Path Tracing is PathTracingSettings::enabled,
    // Hybrid the ray traced effects (RenderDebugSettings::hardwareRayTracing) and Rasterization neither;
    // the Ray Tracing switch is hardwareRayTracing itself. Hybrid, Path Tracing and the switch need a
    // GPU with ray queries, which the backend reports here.
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
    // The car is being filmed from four sides (Tools > Record Quad Cameras), likewise.
    bool quadRecording = false;
};

// A panel the Window menu shows and hides (see EditorWindowManager::BuildPanelMenuEntries).
struct EditorPanelMenuEntry
{
    std::string id;         // command id suffix: "window.<id>"
    std::string windowName; // the ImGui window's name
    std::string icon;
    bool* visible = nullptr;
};

struct EditorWindowCommands
{
    std::span<const EditorPanelMenuEntry> panels;
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
    // Frame Selected: points the viewport camera at the selection, enabled as canFrameSelection says.
    std::function<void()> frameSelection;
    std::function<bool()> canFrameSelection;
    std::function<void()> openPreferences;
    std::function<void()> clearScene;
    std::function<void()> createEntity;
    std::function<void(LightType)> createLight;
    std::function<void()> openSceneSettings;
    std::function<void()> captureViewport;
    std::function<void()> toggleVideoRecording; // Record Viewport: starts or stops
    std::function<void()> toggleQuadRecording;  // Record Quad Cameras: starts or stops
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
