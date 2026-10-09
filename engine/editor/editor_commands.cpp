#include "editor_commands.h"

#include <IconsPhosphor.h>
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
    AddBound(registry, "file.new_scene", "New Scene", "File/New Scene", ICON_PH_FILE, ImGuiMod_Ctrl | ImGuiKey_N, scene.newScene);
    AddBound(registry, "file.open_scene", "Open Scene", "File/Open Scene...", ICON_PH_FOLDER_OPEN, ImGuiMod_Ctrl | ImGuiKey_O, scene.openScene);
    AddBound(registry, "file.save_scene", "Save Scene", "File/Save Scene", ICON_PH_FLOPPY_DISK, ImGuiMod_Ctrl | ImGuiKey_S, scene.saveScene);
    AddBound(registry, "file.save_scene_as", "Save Scene As", "File/Save Scene As...", "", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, scene.saveSceneAs);
    registry.AddSeparator("File");
    AddBound(registry, "file.import_model", "Import Model", "File/Import Model...", ICON_PH_FILE_ARROW_DOWN, ImGuiMod_Ctrl | ImGuiKey_I, scene.importModel);
    registry.AddSeparator("File");
    AddBound(registry, "file.exit", "Exit", "File/Exit", ICON_PH_SIGN_OUT, 0, scene.exit);
}

void RegisterEditCommands(CommandRegistry& registry, const EditorSceneCommands& scene)
{
    // The scene has no undo history nor entity clipboard yet, so the editor binds none of these
    // and they stay disabled, leaving Ctrl+Z, Ctrl+C and the rest to the text fields.
    AddBound(registry, "edit.undo", "Undo", "Edit/Undo", ICON_PH_ARROW_COUNTER_CLOCKWISE, ImGuiMod_Ctrl | ImGuiKey_Z, scene.undo);
    AddBound(registry, "edit.redo", "Redo", "Edit/Redo", ICON_PH_ARROW_CLOCKWISE, ImGuiMod_Ctrl | ImGuiKey_Y, scene.redo);
    registry.AddSeparator("Edit");
    AddBound(registry, "edit.cut", "Cut", "Edit/Cut", ICON_PH_SCISSORS, ImGuiMod_Ctrl | ImGuiKey_X, scene.cut);
    AddBound(registry, "edit.copy", "Copy", "Edit/Copy", ICON_PH_COPY, ImGuiMod_Ctrl | ImGuiKey_C, scene.copy);
    AddBound(registry, "edit.paste", "Paste", "Edit/Paste", ICON_PH_CLIPBOARD_TEXT, ImGuiMod_Ctrl | ImGuiKey_V, scene.paste);
    AddBound(registry, "edit.duplicate", "Duplicate", "Edit/Duplicate", ICON_PH_COPY_SIMPLE, ImGuiMod_Ctrl | ImGuiKey_D, scene.duplicate);
    registry.AddSeparator("Edit");
    AddBound(registry, "edit.delete", "Delete", "Edit/Delete", ICON_PH_TRASH, ImGuiKey_Delete, scene.deleteSelection, scene.hasSelection);
    registry.AddSeparator("Edit");
    AddBound(registry, "edit.preferences", "Preferences", "Edit/Preferences...", ICON_PH_GEAR, ImGuiMod_Ctrl | ImGuiKey_Comma, scene.openPreferences);
}

void RegisterSceneCommands(CommandRegistry& registry, EditorCommandState& state, const EditorSceneCommands& scene)
{
    AddBound(registry, "scene.create_entity", "Create Empty Entity", "Scene/Create Empty Entity", ICON_PH_CUBE, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N, scene.createEntity);
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
    AddBound(registry, "scene.create_light.directional", "Create Directional Light", "Scene/Create Light/Directional", ICON_PH_SUN, 0, createLight(LightType::Directional));
    AddBound(registry, "scene.create_light.point", "Create Point Light", "Scene/Create Light/Point", ICON_PH_LIGHTBULB, 0, createLight(LightType::Point));
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
        registry, "scene.play", "Play", "Scene/Play", ICON_PH_PLAY, ImGuiKey_F5,
        [&state]
        {
            state.playState = state.playState == PlayState::Stopped ? PlayState::Playing : PlayState::Stopped;
        },
        isPlaying);
    Add(
        registry, "scene.pause", "Pause", "Scene/Pause", ICON_PH_PAUSE, ImGuiKey_F6,
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
        registry, "scene.step", "Step", "Scene/Step", ICON_PH_SKIP_FORWARD, ImGuiKey_F10,
        scene.stepSimulation,
        [&state]
        {
            return state.playState == PlayState::Paused;
        });
    registry.AddSeparator("Scene");
    AddBound(registry, "scene.settings", "Scene Settings", "Scene/Scene Settings...", ICON_PH_SLIDERS_HORIZONTAL, 0, scene.openSceneSettings);
    registry.AddSeparator("Scene");
    AddBound(registry, "scene.clear", "Clear Scene", "Scene/Clear Scene", ICON_PH_BROOM, 0, scene.clearScene);

    // Toolbar only: the viewport's R key already toggles combined and scale. The editor UI drives
    // the gizmo from transformTool: Move is the combined translate and rotate gizmo, Rotate the
    // rotate-only one, Scale the scale one.
    AddOption(registry, "tool.move", "Move", "", ICON_PH_ARROWS_OUT_CARDINAL, 0, state.transformTool, TransformTool::Move);
    AddOption(registry, "tool.rotate", "Rotate", "", ICON_PH_ARROWS_CLOCKWISE, 0, state.transformTool, TransformTool::Rotate);
    AddOption(registry, "tool.scale", "Scale", "", ICON_PH_ARROWS_OUT, 0, state.transformTool, TransformTool::Scale);
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
    {GBufferDebugView::Off, "lit", "Lit", ICON_PH_SUN, ImGuiMod_Alt | ImGuiKey_1, false},
    {GBufferDebugView::Albedo, "albedo", "Albedo", ICON_PH_PALETTE, ImGuiMod_Alt | ImGuiKey_2, true},
    {GBufferDebugView::Normal, "normal", "Normal", ICON_PH_COMPASS, ImGuiMod_Alt | ImGuiKey_3, false},
    {GBufferDebugView::GeometricNormal, "geometric_normal", "Geometric Normal", "", 0, false},
    {GBufferDebugView::Surface, "surface", "Metallic, Roughness, Occlusion", ICON_PH_CIRCLE_HALF, ImGuiMod_Alt | ImGuiKey_4, false},
    {GBufferDebugView::Specular, "specular", "Specular", "", 0, false},
    {GBufferDebugView::Emissive, "emissive", "Emissive", ICON_PH_LIGHTBULB, 0, false},
    {GBufferDebugView::Coat, "coat", "Coat and Anisotropy", "", 0, false},
    {GBufferDebugView::Sheen, "sheen", "Sheen", "", 0, false},
    {GBufferDebugView::MotionVectors, "motion_vectors", "Motion Vectors", "", 0, false},
    {GBufferDebugView::AmbientOcclusion, "ambient_occlusion", "Ambient Occlusion", ICON_PH_STACK, ImGuiMod_Alt | ImGuiKey_5, true},
    {GBufferDebugView::Reflections, "reflections", "Screen-Space Reflections", "", 0, false},
    {GBufferDebugView::IndirectDiffuse, "indirect_diffuse", "Screen-Space GI", "", 0, false},
    {GBufferDebugView::LightClusters, "light_clusters", "Light Clusters", "", 0, false},
    {GBufferDebugView::RayTraced, "ray_traced", "DDGI Ray-Traced Scene", ICON_PH_TREE_STRUCTURE, ImGuiMod_Alt | ImGuiKey_6, true},
    {GBufferDebugView::DdgiIrradiance, "ddgi_irradiance", "DDGI Irradiance", "", 0, false},
    {GBufferDebugView::DdgiProbes, "ddgi_probes", "DDGI Probes", "", 0, false},
    {GBufferDebugView::RayTracedShadow, "ray_traced_shadow", "Ray-Traced Sun Shadow", "", 0, true},
    {GBufferDebugView::ProbeOcclusion, "probe_occlusion", "DDGI Probe Occlusion", "", 0, false},
};

void RegisterViewCommands(CommandRegistry& registry, EditorCommandState& state, const EditorSceneCommands& scene)
{
    Add(
        registry, "view.wireframe", "Wireframe", "View/Wireframe", ICON_PH_POLYGON, ImGuiMod_Alt | ImGuiKey_W,
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
    AddBound(registry, "view.frame_selected", "Frame Selected", "View/Frame Selected", ICON_PH_TARGET, ImGuiKey_F, scene.frameSelection, scene.canFrameSelection);
    Add(
        registry, "view.viewport_fullscreen", "Fullscreen Viewport", "View/Fullscreen Viewport", ICON_PH_CORNERS_OUT, ImGuiKey_F11,
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
        registry, "view.gizmos", "Gizmos", "View/Gizmos", ICON_PH_CROSSHAIR, ImGuiMod_Alt | ImGuiKey_G,
        [&state]
        {
            state.gizmos = !state.gizmos;
        },
        [&state]
        {
            return state.gizmos;
        });
    Add(
        registry, "view.driving_hud", "Driving HUD", "View/Driving HUD", ICON_PH_GAUGE, ImGuiMod_Alt | ImGuiKey_H,
        [&state]
        {
            state.drivingHud = !state.drivingHud;
        },
        [&state]
        {
            return state.drivingHud;
        });
    Add(
        registry, "view.minimap", "Minimap", "View/Minimap", ICON_PH_MAP_TRIFOLD, ImGuiMod_Alt | ImGuiKey_M,
        [&state]
        {
            state.minimap = !state.minimap;
        },
        [&state]
        {
            return state.minimap;
        });
    Add(
        registry, "view.viewport_ui", "Viewport UI", "View/Viewport UI", ICON_PH_LAYOUT, ImGuiMod_Alt | ImGuiKey_U,
        [&state]
        {
            state.viewportUi = !state.viewportUi;
        },
        [&state]
        {
            return state.viewportUi;
        });
}

void RegisterRenderCommands(CommandRegistry& registry, EditorCommandState& state, const EditorSceneCommands& scene)
{
    // The editor UI sets the renderer's tone mapping, TAA, pipeline and ray tracing from these. The
    // pipeline modes but Rasterization, and the Ray Tracing switch, need rayTracingSupported.
    const auto rayTracingSupported = [&state]
    {
        return state.rayTracingSupported;
    };
    AddOption(registry, "render.pipeline.rasterization", "Rasterization", "Render/Pipeline/Rasterization", ICON_PH_CUBE, ImGuiMod_Ctrl | ImGuiKey_1, state.pipelineMode, RenderPipelineMode::Rasterization);
    AddOption(registry, "render.pipeline.hybrid", "Hybrid", "Render/Pipeline/Hybrid", ICON_PH_CUBE_TRANSPARENT, ImGuiMod_Ctrl | ImGuiKey_2, state.pipelineMode, RenderPipelineMode::Hybrid, rayTracingSupported);
    AddOption(registry, "render.pipeline.path_tracing", "Path Tracing", "Render/Pipeline/Path Tracing", ICON_PH_MAGIC_WAND, ImGuiMod_Ctrl | ImGuiKey_3, state.pipelineMode, RenderPipelineMode::PathTracing, rayTracingSupported);
    // Path tracing always traces rays, so the switch shows on and cannot be turned off there.
    Add(
        registry, "render.ray_tracing", "Ray Tracing", "Render/Ray Tracing", ICON_PH_LIGHTNING, 0,
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
    AddBound(registry, "render.reload_shaders", "Reload Shaders", "Render/Reload Shaders", ICON_PH_ARROWS_CLOCKWISE, ImGuiMod_Ctrl | ImGuiKey_R, scene.reloadShaders);
}

void RegisterToolsCommands(CommandRegistry& registry, EditorCommandState& state, const EditorSceneCommands& scene)
{
    Add(
        registry, "tools.command_palette", "Command Palette", "Tools/Command Palette...", ICON_PH_TERMINAL_WINDOW, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P,
        [&state]
        {
            state.commandPaletteRequested = true;
        });
    registry.AddSeparator("Tools");
    AddBound(registry, "tools.capture_viewport", "Capture Viewport", "Tools/Capture Viewport", ICON_PH_CAMERA, ImGuiKey_F12, scene.captureViewport);
    // Checked while recording; the same command stops it.
    Add(
        registry, "tools.record_viewport", "Record Viewport", "Tools/Record Viewport", ICON_PH_VIDEO_CAMERA, ImGuiMod_Shift | ImGuiKey_F12,
        scene.toggleVideoRecording,
        [&state]
        {
            return state.videoRecording;
        },
        IfBound(scene.toggleVideoRecording));
    // The car from the front, the rear and both sides at once, composed into one video as it is
    // filmed (Window > Quad Recording sets the cameras up).
    Add(
        registry, "tools.record_quad_cameras", "Record Quad Cameras", "Tools/Record Quad Cameras", ICON_PH_SQUARES_FOUR,
        ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F12,
        scene.toggleQuadRecording,
        [&state]
        {
            return state.quadRecording;
        },
        IfBound(scene.toggleQuadRecording));
    // A still from the viewport's camera at the Photo Mode window's size; the viewport keeps its own.
    AddBound(registry, "tools.take_photo", "Take Photo", "Tools/Take Photo", ICON_PH_APERTURE, ImGuiMod_Ctrl | ImGuiKey_F12, scene.takePhoto);
    // Shaders are compiled with the build, not by the editor, so there is no log or cache to show.
    AddBound(registry, "tools.shader_log", "Shader Compiler Log", "Tools/Shader Compiler Log...", ICON_PH_FILE_TEXT, 0, scene.showShaderLog);
    registry.AddSeparator("Tools");
    AddBound(registry, "tools.clear_shader_cache", "Clear Shader Cache", "Tools/Clear Shader Cache", ICON_PH_TRASH, 0, scene.clearShaderCache);
}

void RegisterWindowCommands(CommandRegistry& registry, EditorCommandState& state, const EditorWindowCommands& window)
{
    for (const EditorPanelMenuEntry& panel : window.panels)
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
    for (const EditorPanelMenuEntry& panel : window.panels)
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
    Add(registry, "window.reset_layout", "Reset Layout", "Window/Reset Layout", ICON_PH_SQUARE_SPLIT_HORIZONTAL, 0, window.resetLayout);
    Add(
        registry, "window.auto_layout", "Auto Layout", "Window/Auto Layout", ICON_PH_FRAME_CORNERS, 0,
        [&state]
        {
            state.autoLayout = !state.autoLayout;
        },
        [&state]
        {
            return state.autoLayout;
        });
}

void RegisterHelpCommands(CommandRegistry& registry, const EditorSceneCommands& scene)
{
    AddBound(registry, "help.documentation", "Documentation", "Help/Documentation", ICON_PH_BOOK, ImGuiKey_F1, scene.showDocumentation);
    AddBound(registry, "help.keyboard_shortcuts", "Keyboard Shortcuts", "Help/Keyboard Shortcuts...", ICON_PH_KEYBOARD, 0, scene.showKeyboardShortcuts);
    registry.AddSeparator("Help");
    AddBound(registry, "help.about", "About MiniEngine", "Help/About MiniEngine...", ICON_PH_INFO, 0, scene.showAbout);
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
    RegisterViewCommands(registry, state, scene);
    RegisterRenderCommands(registry, state, scene);
    RegisterToolsCommands(registry, state, scene);
    RegisterWindowCommands(registry, state, window);
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
