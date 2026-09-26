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

void Todo()
{
}

void RegisterFileCommands(CommandRegistry& registry)
{
    Add(registry, "file.new_scene", "New Scene", "File/New Scene", ICON_FA_FILE, ImGuiMod_Ctrl | ImGuiKey_N, Todo);
    Add(registry, "file.open_scene", "Open Scene", "File/Open Scene...", ICON_FA_FOLDER_OPEN, ImGuiMod_Ctrl | ImGuiKey_O, Todo);
    Add(registry, "file.save_scene", "Save Scene", "File/Save Scene", ICON_FA_FLOPPY_DISK, ImGuiMod_Ctrl | ImGuiKey_S, Todo);
    Add(registry, "file.save_scene_as", "Save Scene As", "File/Save Scene As...", "", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, Todo);
    registry.AddSeparator("File");
    Add(registry, "file.import_model", "Import Model", "File/Import Model...", ICON_FA_FILE_IMPORT, ImGuiMod_Ctrl | ImGuiKey_I, Todo);
    registry.AddSeparator("File");
    Add(registry, "file.exit", "Exit", "File/Exit", ICON_FA_RIGHT_FROM_BRACKET, 0, Todo);
}

void RegisterEditCommands(CommandRegistry& registry)
{
    Add(registry, "edit.undo", "Undo", "Edit/Undo", ICON_FA_ROTATE_LEFT, ImGuiMod_Ctrl | ImGuiKey_Z, Todo);
    Add(registry, "edit.redo", "Redo", "Edit/Redo", ICON_FA_ROTATE_RIGHT, ImGuiMod_Ctrl | ImGuiKey_Y, Todo);
    registry.AddSeparator("Edit");
    Add(registry, "edit.cut", "Cut", "Edit/Cut", ICON_FA_SCISSORS, ImGuiMod_Ctrl | ImGuiKey_X, Todo);
    Add(registry, "edit.copy", "Copy", "Edit/Copy", ICON_FA_COPY, ImGuiMod_Ctrl | ImGuiKey_C, Todo);
    Add(registry, "edit.paste", "Paste", "Edit/Paste", ICON_FA_PASTE, ImGuiMod_Ctrl | ImGuiKey_V, Todo);
    Add(registry, "edit.duplicate", "Duplicate", "Edit/Duplicate", ICON_FA_CLONE, ImGuiMod_Ctrl | ImGuiKey_D, Todo);
    registry.AddSeparator("Edit");
    Add(registry, "edit.delete", "Delete", "Edit/Delete", ICON_FA_TRASH_CAN, ImGuiKey_Delete, Todo);
    registry.AddSeparator("Edit");
    Add(registry, "edit.preferences", "Preferences", "Edit/Preferences...", ICON_FA_GEAR, ImGuiMod_Ctrl | ImGuiKey_Comma, Todo);
}

void RegisterSceneCommands(CommandRegistry& registry, EditorCommandState& state)
{
    Add(registry, "scene.create_entity", "Create Empty Entity", "Scene/Create Empty Entity", ICON_FA_CUBE, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N, Todo);
    Add(registry, "scene.create_light.directional", "Create Directional Light", "Scene/Create Light/Directional", ICON_FA_SUN, 0, Todo);
    Add(registry, "scene.create_light.point", "Create Point Light", "Scene/Create Light/Point", ICON_FA_LIGHTBULB, 0, Todo);
    Add(registry, "scene.create_light.spot", "Create Spot Light", "Scene/Create Light/Spot", "", 0, Todo);
    Add(registry, "scene.create_light.area", "Create Area Light", "Scene/Create Light/Area", "", 0, Todo);
    registry.AddSeparator("Scene");

    // Play controls. Play stops again while playing; Step advances one frame while paused.
    const auto isPlaying = [&state]
    {
        return state.playState != PlayState::Stopped;
    };
    Add(
        registry, "scene.play", "Play", "Scene/Play", ICON_FA_PLAY, ImGuiKey_F5,
        [&state]
        {
            // TODO: start or stop the simulation.
            state.playState = state.playState == PlayState::Stopped ? PlayState::Playing : PlayState::Stopped;
        },
        isPlaying);
    Add(
        registry, "scene.pause", "Pause", "Scene/Pause", ICON_FA_PAUSE, ImGuiKey_F6,
        [&state]
        {
            // TODO: pause or resume the simulation.
            state.playState = state.playState == PlayState::Paused ? PlayState::Playing : PlayState::Paused;
        },
        [&state]
        {
            return state.playState == PlayState::Paused;
        },
        isPlaying);
    Add(
        registry, "scene.step", "Step", "Scene/Step", ICON_FA_FORWARD_STEP, ImGuiKey_F10,
        Todo, // TODO: advance the paused simulation by one frame.
        {},
        [&state]
        {
            return state.playState == PlayState::Paused;
        });
    registry.AddSeparator("Scene");
    Add(registry, "scene.settings", "Scene Settings", "Scene/Scene Settings...", ICON_FA_SLIDERS, 0, Todo);
    registry.AddSeparator("Scene");
    Add(registry, "scene.clear", "Clear Scene", "Scene/Clear Scene", ICON_FA_BROOM, 0, Todo);

    // Toolbar only: the viewport's R key already toggles translate and rotate.
    // TODO: drive the gizmo operation (IEditorWorld::GetGizmoSettings) from transformTool.
    AddOption(registry, "tool.move", "Move", "", ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, 0, state.transformTool, TransformTool::Move);
    AddOption(registry, "tool.rotate", "Rotate", "", ICON_FA_ROTATE, 0, state.transformTool, TransformTool::Rotate);
    AddOption(registry, "tool.scale", "Scale", "", ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, 0, state.transformTool, TransformTool::Scale);
}

void RegisterViewCommands(CommandRegistry& registry, EditorCommandState& state)
{
    // TODO: every View command should reach the renderer's debug settings.
    Add(
        registry, "view.wireframe", "Wireframe", "View/Wireframe", ICON_FA_DRAW_POLYGON, ImGuiMod_Alt | ImGuiKey_W,
        [&state]
        {
            state.wireframe = !state.wireframe;
        },
        [&state]
        {
            return state.wireframe;
        });
    registry.AddSeparator("View");
    AddOption(registry, "view.debug.lit", "Lit", "View/Debug View/Lit", ICON_FA_SUN, ImGuiMod_Alt | ImGuiKey_1, state.debugView, ViewportDebugView::Lit);
    AddOption(registry, "view.debug.albedo", "Albedo", "View/Debug View/Albedo", ICON_FA_PALETTE, ImGuiMod_Alt | ImGuiKey_2, state.debugView, ViewportDebugView::Albedo);
    AddOption(registry, "view.debug.normal", "Normal", "View/Debug View/Normal", ICON_FA_COMPASS, ImGuiMod_Alt | ImGuiKey_3, state.debugView, ViewportDebugView::Normal);
    AddOption(registry, "view.debug.roughness", "Roughness", "View/Debug View/Roughness", ICON_FA_CIRCLE_HALF_STROKE, ImGuiMod_Alt | ImGuiKey_4, state.debugView, ViewportDebugView::Roughness);
    AddOption(registry, "view.debug.depth", "Depth", "View/Debug View/Depth", ICON_FA_LAYER_GROUP, ImGuiMod_Alt | ImGuiKey_5, state.debugView, ViewportDebugView::Depth);
    // The BVH only exists when ray tracing does.
    AddOption(
        registry, "view.debug.bvh", "BVH", "View/Debug View/BVH", ICON_FA_SITEMAP, ImGuiMod_Alt | ImGuiKey_6, state.debugView, ViewportDebugView::Bvh,
        [&state]
        {
            return state.rayTracingSupported;
        });
    registry.AddSeparator("View");
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

void RegisterRenderCommands(CommandRegistry& registry, EditorCommandState& state)
{
    // TODO: every Render command should reach the renderer's pipeline settings.
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
    AddOption(registry, "render.tone_mapping.gt7", "GT7", "Render/Tone Mapping/GT7", "", 0, state.toneMapping, ToneMappingMode::Gt7);
    AddOption(registry, "render.tone_mapping.pbr_neutral", "PBR Neutral", "Render/Tone Mapping/PBR Neutral", "", 0, state.toneMapping, ToneMappingMode::PbrNeutral);
    AddOption(registry, "render.tone_mapping.none", "No Tone Mapping", "Render/Tone Mapping/None", "", 0, state.toneMapping, ToneMappingMode::None);
    AddOption(registry, "render.anti_aliasing.taa", "TAA", "Render/Anti-Aliasing/TAA", "", 0, state.antiAliasing, AntiAliasingMode::Taa);
    AddOption(registry, "render.anti_aliasing.none", "No Anti-Aliasing", "Render/Anti-Aliasing/None", "", 0, state.antiAliasing, AntiAliasingMode::None);
    registry.AddSeparator("Render");
    Add(registry, "render.reload_shaders", "Reload Shaders", "Render/Reload Shaders", ICON_FA_ARROWS_ROTATE, ImGuiMod_Ctrl | ImGuiKey_R, Todo);
}

void RegisterToolsCommands(CommandRegistry& registry, EditorCommandState& state)
{
    Add(
        registry, "tools.command_palette", "Command Palette", "Tools/Command Palette...", ICON_FA_TERMINAL, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P,
        [&state]
        {
            state.commandPaletteRequested = true;
        });
    registry.AddSeparator("Tools");
    Add(registry, "tools.capture_viewport", "Capture Viewport", "Tools/Capture Viewport", ICON_FA_CAMERA, ImGuiKey_F12, Todo);
    Add(registry, "tools.shader_log", "Shader Compiler Log", "Tools/Shader Compiler Log...", ICON_FA_FILE_LINES, 0, Todo);
    registry.AddSeparator("Tools");
    Add(registry, "tools.clear_shader_cache", "Clear Shader Cache", "Tools/Clear Shader Cache", ICON_FA_TRASH_CAN, 0, Todo);
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

void RegisterHelpCommands(CommandRegistry& registry)
{
    Add(registry, "help.documentation", "Documentation", "Help/Documentation", ICON_FA_BOOK, ImGuiKey_F1, Todo);
    Add(registry, "help.keyboard_shortcuts", "Keyboard Shortcuts", "Help/Keyboard Shortcuts...", ICON_FA_KEYBOARD, 0, Todo);
    registry.AddSeparator("Help");
    Add(registry, "help.about", "About MiniEngine", "Help/About MiniEngine...", ICON_FA_CIRCLE_INFO, 0, Todo);
}
}

void RegisterEditorCommands(CommandRegistry& registry, EditorCommandState& state, const EditorWindowCommands& window)
{
    RegisterFileCommands(registry);
    RegisterEditCommands(registry);
    RegisterSceneCommands(registry, state);
    RegisterViewCommands(registry, state);
    RegisterRenderCommands(registry, state);
    RegisterToolsCommands(registry, state);
    RegisterWindowCommands(registry, window);
    RegisterHelpCommands(registry);
}

ToolbarLayout BuildEditorToolbarLayout()
{
    ToolbarLayout layout;
    layout.left = {
        {ToolbarItem::Button("tool.move"), ToolbarItem::Button("tool.rotate"), ToolbarItem::Button("tool.scale")},
    };
    layout.center = {
        {ToolbarItem::Button("scene.play"), ToolbarItem::Button("scene.pause"), ToolbarItem::Button("scene.step")},
    };
    layout.right = {
        {ToolbarItem::Dropdown(
             "Pipeline",
             {"render.pipeline.rasterization", "render.pipeline.hybrid", "render.pipeline.path_tracing"},
             150.0f),
         ToolbarItem::Button("render.ray_tracing")},
        {ToolbarItem::Dropdown(
            "Debug View",
            {"view.debug.lit", "view.debug.albedo", "view.debug.normal", "view.debug.roughness", "view.debug.depth", "view.debug.bvh"},
            130.0f)},
    };
    return layout;
}
}
