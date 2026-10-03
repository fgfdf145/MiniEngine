#include "editor_commands.h"

#include <IconsFontAwesome6.h>
#include <imgui.h>

#include <utility>

namespace me
{

namespace
{
void Add(
    CommandRegistry& registry,
    std::string id,
    std::string label,
    std::string menuPath,
    std::string icon,
    ImGuiKeyChord shortcut,
    std::function<void()> execute,
    std::function<bool()> isChecked = {},
    std::function<bool()> isEnabled = {})
{
    const bool registered = registry.Register(Command{
        std::move(id),
        std::move(label),
        std::move(menuPath),
        std::move(icon),
        shortcut,
        std::move(execute),
        std::move(isChecked),
        std::move(isEnabled)});
    IM_ASSERT(registered && "Duplicate command id or invalid menu path");
    static_cast<void>(registered);
}

// One option of a radio group: checked while `field` holds `value`, and selecting it stores `value`.
template <typename T>
void AddOption(
    CommandRegistry& registry,
    std::string id,
    std::string label,
    std::string menuPath,
    std::string icon,
    ImGuiKeyChord shortcut,
    T& field,
    T value,
    std::function<bool()> isEnabled = {})
{
    Add(
        registry,
        std::move(id),
        std::move(label),
        std::move(menuPath),
        std::move(icon),
        shortcut,
        [&field, value]
        {
            field = value;
        },
        [&field, value]
        {
            return field == value;
        },
        std::move(isEnabled));
}

// Enabled only when the editor gave the command something to run, and then as `isEnabled` says.
std::function<bool()> IfBound(const std::function<void()>& execute, std::function<bool()> isEnabled = {})
{
    if (!execute)
    {
        return []
        {
            return false;
        };
    }
    return isEnabled;
}

void AddBound(
    CommandRegistry& registry,
    std::string id,
    std::string label,
    std::string menuPath,
    std::string icon,
    ImGuiKeyChord shortcut,
    const std::function<void()>& execute,
    std::function<bool()> isEnabled = {})
{
    Add(registry, std::move(id), std::move(label), std::move(menuPath), std::move(icon), shortcut, execute, {}, IfBound(execute, std::move(isEnabled)));
}

void RegisterFileCommands(CommandRegistry& registry, const EditorSceneCommands& scene)
{
    AddBound(registry, "file.new_scene", "New Scene", "File/New Scene", ICON_FA_FILE, ImGuiMod_Ctrl | ImGuiKey_N, scene.newScene);
    AddBound(registry, "file.open_scene", "Open Scene", "File/Open Scene...", ICON_FA_FOLDER_OPEN, ImGuiMod_Ctrl | ImGuiKey_O, scene.openScene);
    AddBound(registry, "file.save_scene", "Save Scene", "File/Save Scene", ICON_FA_FLOPPY_DISK, ImGuiMod_Ctrl | ImGuiKey_S, scene.saveScene);
    AddBound(registry, "file.save_scene_as", "Save Scene As", "File/Save Scene As...", "", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, scene.saveSceneAs);
    registry.AddSeparator("File");
    AddBound(registry, "file.import_model", "Import Model", "File/Import Model...", ICON_FA_FILE_IMPORT, ImGuiMod_Ctrl | ImGuiKey_I, scene.importModel);
    registry.AddSeparator("File");
    AddBound(registry, "file.exit", "Exit", "File/Exit", ICON_FA_RIGHT_FROM_BRACKET, 0, scene.exit);
}

void RegisterEditCommands(CommandRegistry& registry, const EditorSceneCommands& scene)
{
    // The scene has no undo history nor entity clipboard yet, so the editor binds none of these
    // and they stay disabled, leaving Ctrl+Z, Ctrl+C and the rest to the text fields.
    AddBound(registry, "edit.undo", "Undo", "Edit/Undo", ICON_FA_ROTATE_LEFT, ImGuiMod_Ctrl | ImGuiKey_Z, scene.undo);
    AddBound(registry, "edit.redo", "Redo", "Edit/Redo", ICON_FA_ROTATE_RIGHT, ImGuiMod_Ctrl | ImGuiKey_Y, scene.redo);
    registry.AddSeparator("Edit");
    AddBound(registry, "edit.cut", "Cut", "Edit/Cut", ICON_FA_SCISSORS, ImGuiMod_Ctrl | ImGuiKey_X, scene.cut);
    AddBound(registry, "edit.copy", "Copy", "Edit/Copy", ICON_FA_COPY, ImGuiMod_Ctrl | ImGuiKey_C, scene.copy);
    AddBound(registry, "edit.paste", "Paste", "Edit/Paste", ICON_FA_PASTE, ImGuiMod_Ctrl | ImGuiKey_V, scene.paste);
    AddBound(registry, "edit.duplicate", "Duplicate", "Edit/Duplicate", ICON_FA_CLONE, ImGuiMod_Ctrl | ImGuiKey_D, scene.duplicate);
    registry.AddSeparator("Edit");
    AddBound(registry, "edit.delete", "Delete", "Edit/Delete", ICON_FA_TRASH_CAN, ImGuiKey_Delete, scene.deleteSelection, scene.hasSelection);
    registry.AddSeparator("Edit");
    AddBound(registry, "edit.preferences", "Preferences", "Edit/Preferences...", ICON_FA_GEAR, ImGuiMod_Ctrl | ImGuiKey_Comma, scene.openPreferences);
}

void RegisterSceneCommands(CommandRegistry& registry, EditorCommandState& state, const EditorSceneCommands& scene)
{
    AddBound(registry, "scene.create_entity", "Create Empty Entity", "Scene/Create Empty Entity", ICON_FA_CUBE, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N, scene.createEntity);
    const auto createLight = [&scene](LightType type) -> std::function<void()>
    {
        if (!scene.createLight)
        {
            return {};
        }
        return [createLight = scene.createLight, type]
        {
            createLight(type);
        };
    };
    AddBound(registry, "scene.create_light.directional", "Create Directional Light", "Scene/Create Light/Directional", ICON_FA_SUN, 0, createLight(LightType::Directional));
    AddBound(registry, "scene.create_light.point", "Create Point Light", "Scene/Create Light/Point", ICON_FA_LIGHTBULB, 0, createLight(LightType::Point));
    AddBound(registry, "scene.create_light.spot", "Create Spot Light", "Scene/Create Light/Spot", "", 0, createLight(LightType::Spot));
    AddBound(registry, "scene.create_light.area", "Create Area Light", "Scene/Create Light/Area", "", 0, createLight(LightType::Area));
    registry.AddSeparator("Scene");

    // Play controls: Play drives the selected model as a car and stops again while playing; Step
    // advances the paused simulation by one physics step. The editor UI turns playState changes into
    // VehicleDriveService actions and reads the state back from it each frame.
    const auto isPlaying = [&state]
    {
        return state.playState != PlayState::Stopped;
    };
    Add(
        registry, "scene.play", "Play", "Scene/Play", ICON_FA_PLAY, ImGuiKey_F5,
        [&state]
        {
            state.playState = state.playState == PlayState::Stopped ? PlayState::Playing : PlayState::Stopped;
        },
        isPlaying);
    Add(
        registry, "scene.pause", "Pause", "Scene/Pause", ICON_FA_PAUSE, ImGuiKey_F6,
        [&state]
        {
            state.playState = state.playState == PlayState::Paused ? PlayState::Playing : PlayState::Paused;
        },
        [&state]
        {
            return state.playState == PlayState::Paused;
        },
        isPlaying);
    AddBound(
        registry, "scene.step", "Step", "Scene/Step", ICON_FA_FORWARD_STEP, ImGuiKey_F10,
        scene.stepSimulation,
        [&state]
        {
            return state.playState == PlayState::Paused;
        });
    registry.AddSeparator("Scene");
    AddBound(registry, "scene.settings", "Scene Settings", "Scene/Scene Settings...", ICON_FA_SLIDERS, 0, scene.openSceneSettings);
    registry.AddSeparator("Scene");
    AddBound(registry, "scene.clear", "Clear Scene", "Scene/Clear Scene", ICON_FA_BROOM, 0, scene.clearScene);

    // Toolbar only: the viewport's R key already toggles combined and scale. The editor UI drives
    // the gizmo from transformTool: Move is the combined translate and rotate gizmo, Rotate the
    // rotate-only one, Scale the scale one.
    AddOption(registry, "tool.move", "Move", "", ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, 0, state.transformTool, TransformTool::Move);
    AddOption(registry, "tool.rotate", "Rotate", "", ICON_FA_ROTATE, 0, state.transformTool, TransformTool::Rotate);
    AddOption(registry, "tool.scale", "Scale", "", ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, 0, state.transformTool, TransformTool::Scale);
}

// Every view the tone mapping pass has (GBufferDebugView), in the order the View menu lists them.
struct DebugViewCommand
{
    GBufferDebugView view;
    const char* idSuffix;
    const char* label;
    const char* icon;
    ImGuiKeyChord shortcut;
    bool separatorBefore; // starts a group in the menu
};

constexpr DebugViewCommand kDebugViewCommands[] = {
    {GBufferDebugView::Off, "lit", "Lit", ICON_FA_SUN, ImGuiMod_Alt | ImGuiKey_1, false},
    {GBufferDebugView::Albedo, "albedo", "Albedo", ICON_FA_PALETTE, ImGuiMod_Alt | ImGuiKey_2, true},
    {GBufferDebugView::Normal, "normal", "Normal", ICON_FA_COMPASS, ImGuiMod_Alt | ImGuiKey_3, false},
    {GBufferDebugView::GeometricNormal, "geometric_normal", "Geometric Normal", "", 0, false},
    {GBufferDebugView::Surface, "surface", "Metallic, Roughness, Occlusion", ICON_FA_CIRCLE_HALF_STROKE, ImGuiMod_Alt | ImGuiKey_4, false},
    {GBufferDebugView::Specular, "specular", "Specular", "", 0, false},
    {GBufferDebugView::Emissive, "emissive", "Emissive", ICON_FA_LIGHTBULB, 0, false},
    {GBufferDebugView::Coat, "coat", "Coat and Anisotropy", "", 0, false},
    {GBufferDebugView::Sheen, "sheen", "Sheen", "", 0, false},
    {GBufferDebugView::MotionVectors, "motion_vectors", "Motion Vectors", "", 0, false},
    {GBufferDebugView::AmbientOcclusion, "ambient_occlusion", "Ambient Occlusion", ICON_FA_LAYER_GROUP, ImGuiMod_Alt | ImGuiKey_5, true},
    {GBufferDebugView::Reflections, "reflections", "Screen-Space Reflections", "", 0, false},
    {GBufferDebugView::IndirectDiffuse, "indirect_diffuse", "Screen-Space GI", "", 0, false},
    {GBufferDebugView::LightClusters, "light_clusters", "Light Clusters", "", 0, false},
    {GBufferDebugView::RayTraced, "ray_traced", "DDGI Ray-Traced Scene", ICON_FA_SITEMAP, ImGuiMod_Alt | ImGuiKey_6, true},
    {GBufferDebugView::DdgiIrradiance, "ddgi_irradiance", "DDGI Irradiance", "", 0, false},
    {GBufferDebugView::DdgiProbes, "ddgi_probes", "DDGI Probes", "", 0, false},
};

void RegisterViewCommands(CommandRegistry& registry, EditorCommandState& state)
{
    Add(
        registry, "view.wireframe", "Wireframe", "View/Wireframe", ICON_FA_DRAW_POLYGON, ImGuiMod_Alt | ImGuiKey_W,
        [&state]
        {
            state.wireframe = !state.wireframe;
        },
        [&state]
        {
            return state.wireframe;
        },
        [&state]
        {
            return state.wireframeSupported;
        });
    registry.AddSeparator("View");
    // The forward-only order writes no G-buffer: only the shaded image is there to show.
    const auto gbufferAvailable = [&state]
    {
        return state.gbufferAvailable;
    };
    for (const DebugViewCommand& command : kDebugViewCommands)
    {
        if (command.separatorBefore)
        {
            registry.AddSeparator("View/Debug View");
        }
        AddOption(
            registry, DebugViewCommandId(command.view), command.label, std::string("View/Debug View/") + command.label, command.icon,
            command.shortcut, state.debugView, command.view,
            command.view == GBufferDebugView::Off ? std::function<bool()>{} : std::function<bool()>(gbufferAvailable));
    }
    registry.AddSeparator("View");
    Add(
        registry, "view.viewport_fullscreen", "Fullscreen Viewport", "View/Fullscreen Viewport", ICON_FA_EXPAND, ImGuiKey_F11,
        [&state]
        {
            state.viewportFullscreen = !state.viewportFullscreen;
        },
        [&state]
        {
            return state.viewportFullscreen;
        });
    // The viewport's transform gizmo and light gizmos.
    Add(
        registry, "view.gizmos", "Gizmos", "View/Gizmos", ICON_FA_CROSSHAIRS, ImGuiMod_Alt | ImGuiKey_G,
        [&state]
        {
            state.gizmos = !state.gizmos;
        },
        [&state]
        {
            return state.gizmos;
        });
}

void RegisterRenderCommands(CommandRegistry& registry, EditorCommandState& state, const EditorSceneCommands& scene)
{
    // The editor UI sets the renderer's tone mapping and TAA from these. The pipeline modes and ray
    // tracing need rayTracingSupported, which stays false until the renderer has them.
    const auto rayTracingSupported = [&state]
    {
        return state.rayTracingSupported;
    };
    AddOption(registry, "render.pipeline.rasterization", "Rasterization", "Render/Pipeline/Rasterization", ICON_FA_CUBE, ImGuiMod_Ctrl | ImGuiKey_1, state.pipelineMode, RenderPipelineMode::Rasterization);
    AddOption(registry, "render.pipeline.hybrid", "Hybrid", "Render/Pipeline/Hybrid", ICON_FA_CUBES, ImGuiMod_Ctrl | ImGuiKey_2, state.pipelineMode, RenderPipelineMode::Hybrid, rayTracingSupported);
    AddOption(registry, "render.pipeline.path_tracing", "Path Tracing", "Render/Pipeline/Path Tracing", ICON_FA_WAND_MAGIC_SPARKLES, ImGuiMod_Ctrl | ImGuiKey_3, state.pipelineMode, RenderPipelineMode::PathTracing, rayTracingSupported);
    // Path tracing always traces rays, so the switch shows on and cannot be turned off there.
    Add(
        registry, "render.ray_tracing", "Ray Tracing", "Render/Ray Tracing", ICON_FA_BOLT, 0,
        [&state]
        {
            state.rayTracing = !state.rayTracing;
        },
        [&state]
        {
            return state.rayTracing || state.pipelineMode == RenderPipelineMode::PathTracing;
        },
        [&state]
        {
            return state.rayTracingSupported && state.pipelineMode != RenderPipelineMode::PathTracing;
        });
    registry.AddSeparator("Render");
    // The Khronos reference view (Graphics Debug) always uses PBR Neutral.
    const auto toneMappingSelectable = [&state]
    {
        return !state.khronosReference;
    };
    AddOption(registry, "render.tone_mapping.gt7", "GT7", "Render/Tone Mapping/GT7", "", 0, state.toneMapping, ToneMapper::Gt7, toneMappingSelectable);
    AddOption(registry, "render.tone_mapping.pbr_neutral", "PBR Neutral", "Render/Tone Mapping/PBR Neutral", "", 0, state.toneMapping, ToneMapper::PbrNeutral, toneMappingSelectable);
    AddOption(registry, "render.tone_mapping.none", "No Tone Mapping", "Render/Tone Mapping/None", "", 0, state.toneMapping, ToneMapper::None, toneMappingSelectable);
    AddOption(registry, "render.anti_aliasing.taa", "TAA", "Render/Anti-Aliasing/TAA", "", 0, state.antiAliasing, AntiAliasingMode::Taa);
    AddOption(registry, "render.anti_aliasing.none", "No Anti-Aliasing", "Render/Anti-Aliasing/None", "", 0, state.antiAliasing, AntiAliasingMode::None);
    registry.AddSeparator("Render");
    // The passes load their SPIR-V once, when they are built; the renderer cannot rebuild them yet.
    AddBound(registry, "render.reload_shaders", "Reload Shaders", "Render/Reload Shaders", ICON_FA_ARROWS_ROTATE, ImGuiMod_Ctrl | ImGuiKey_R, scene.reloadShaders);
}

void RegisterToolsCommands(CommandRegistry& registry, EditorCommandState& state, const EditorSceneCommands& scene)
{
    Add(
        registry, "tools.command_palette", "Command Palette", "Tools/Command Palette...", ICON_FA_TERMINAL, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P,
        [&state]
        {
            state.commandPaletteRequested = true;
        });
    registry.AddSeparator("Tools");
    AddBound(registry, "tools.capture_viewport", "Capture Viewport", "Tools/Capture Viewport", ICON_FA_CAMERA, ImGuiKey_F12, scene.captureViewport);
    // Checked while recording; the same command stops it.
    Add(
        registry, "tools.record_viewport", "Record Viewport", "Tools/Record Viewport", ICON_FA_VIDEO, ImGuiMod_Shift | ImGuiKey_F12,
        scene.toggleVideoRecording,
        [&state]
        {
            return state.videoRecording;
        },
        IfBound(scene.toggleVideoRecording));
    // Shaders are compiled with the build, not by the editor, so there is no log or cache to show.
    AddBound(registry, "tools.shader_log", "Shader Compiler Log", "Tools/Shader Compiler Log...", ICON_FA_FILE_LINES, 0, scene.showShaderLog);
    registry.AddSeparator("Tools");
    AddBound(registry, "tools.clear_shader_cache", "Clear Shader Cache", "Tools/Clear Shader Cache", ICON_FA_TRASH_CAN, 0, scene.clearShaderCache);
}

void RegisterWindowCommands(CommandRegistry& registry, const EditorWindowCommands& window)
{
    for (const EditorPanel& panel : window.panels)
    {
        bool* visible = panel.visible;
        Add(
            registry, "window." + panel.id, panel.windowName, "Window/" + panel.windowName, panel.icon, 0,
            [visible]
            {
                *visible = !*visible;
            },
            [visible]
            {
                return *visible;
            });
    }
    registry.AddSeparator("Window");

    std::vector<bool*> visibleFlags;
    for (const EditorPanel& panel : window.panels)
    {
        visibleFlags.push_back(panel.visible);
    }
    Add(
        registry, "window.show_all", "Show All Panels", "Window/Show All Panels", "", 0,
        [visibleFlags]
        {
            for (bool* visible : visibleFlags)
            {
                *visible = true;
            }
        });
    Add(registry, "window.reset_layout", "Reset Layout", "Window/Reset Layout", ICON_FA_TABLE_COLUMNS, 0, window.resetLayout);
}

void RegisterHelpCommands(CommandRegistry& registry, const EditorSceneCommands& scene)
{
    AddBound(registry, "help.documentation", "Documentation", "Help/Documentation", ICON_FA_BOOK, ImGuiKey_F1, scene.showDocumentation);
    AddBound(registry, "help.keyboard_shortcuts", "Keyboard Shortcuts", "Help/Keyboard Shortcuts...", ICON_FA_KEYBOARD, 0, scene.showKeyboardShortcuts);
    registry.AddSeparator("Help");
    AddBound(registry, "help.about", "About MiniEngine", "Help/About MiniEngine...", ICON_FA_CIRCLE_INFO, 0, scene.showAbout);
}
}

void RegisterEditorCommands(
    CommandRegistry& registry,
    EditorCommandState& state,
    const EditorWindowCommands& window,
    const EditorSceneCommands& scene)
{
    RegisterFileCommands(registry, scene);
    RegisterEditCommands(registry, scene);
    RegisterSceneCommands(registry, state, scene);
    RegisterViewCommands(registry, state);
    RegisterRenderCommands(registry, state, scene);
    RegisterToolsCommands(registry, state, scene);
    RegisterWindowCommands(registry, window);
    RegisterHelpCommands(registry, scene);
}

std::string DebugViewCommandId(GBufferDebugView view)
{
    for (const DebugViewCommand& command : kDebugViewCommands)
    {
        if (command.view == view)
        {
            return std::string("view.debug.") + command.idSuffix;
        }
    }
    return {};
}

ToolbarLayout BuildEditorToolbarLayout()
{
    std::vector<std::string> debugViews;
    for (const DebugViewCommand& command : kDebugViewCommands)
    {
        debugViews.push_back(DebugViewCommandId(command.view));
    }

    ToolbarLayout layout;
    layout.left = {
        {ToolbarItem::Button("tool.move"), ToolbarItem::Button("tool.rotate"), ToolbarItem::Button("tool.scale")},
    };
    layout.center = {
        {ToolbarItem::Button("scene.play"), ToolbarItem::Button("scene.pause"), ToolbarItem::Button("scene.step")},
        {ToolbarItem::Button("tools.record_viewport")},
    };
    layout.right = {
        {ToolbarItem::Dropdown(
             "Pipeline",
             {"render.pipeline.rasterization", "render.pipeline.hybrid", "render.pipeline.path_tracing"},
             150.0f),
         ToolbarItem::Button("render.ray_tracing")},
        {ToolbarItem::Dropdown("Debug View", debugViews, 170.0f)},
    };
    return layout;
}
}
