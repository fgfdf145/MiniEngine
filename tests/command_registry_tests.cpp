#include <engine/editor/command_registry.h>
#include <engine/editor/editor_commands.h>
#include <engine/editor/engine_settings.h>

#include <imgui.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Runs the command with this id when it exists and is enabled; returns whether it ran.
bool Run(const CommandRegistry& registry, std::string_view id)
{
    const Command* command = registry.Find(id);
    return command != nullptr && registry.Execute(*command);
}

const CommandMenuNode* FindMenu(const CommandMenuNode& parent, const std::string& label)
{
    for (const CommandMenuNode& child : parent.children)
    {
        if (child.kind == CommandMenuNode::Kind::Menu && child.label == label)
        {
            return &child;
        }
    }
    return nullptr;
}

void CollectItems(const CommandRegistry& registry, const CommandMenuNode& node, int depth, int& maxDepth, std::vector<const Command*>& items)
{
    for (const CommandMenuNode& child : node.children)
    {
        if (child.kind == CommandMenuNode::Kind::Item)
        {
            items.push_back(&registry.GetCommands()[child.commandIndex]);
            maxDepth = depth > maxDepth ? depth : maxDepth;
        }
        else if (child.kind == CommandMenuNode::Kind::Menu)
        {
            CollectItems(registry, child, depth + 1, maxDepth, items);
        }
    }
}

// Every window's open state survives the settings file, the ones added after the first seven too.
void TestWindowStatesSurviveTheSettingsFile()
{
    EngineSettings saved;
    saved.editorUi.windows.open = {{"asset_manager", false}, {"camera", true}, {"suspension_rigs", true}, {"vehicle", true}};
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_window_states_test.json";
    std::string error;
    Require(SaveEngineSettings(path, saved, error), ("the settings save: " + error).c_str());
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), ("the settings load: " + error).c_str());
    std::filesystem::remove(path);
    Require(loaded.editorUi.windows.open == saved.editorUi.windows.open, "every window's open state comes back");
}

// The display calibration survives the settings file, the output mode (an enum) as its number.
void TestDisplayCalibrationSurvivesTheSettingsFile()
{
    EngineSettings saved;
    DisplaySettings& display = saved.view.renderDebug.display;
    display.outputMode = DisplayOutputMode::Hdr;
    display.calibrated = true;
    display.maxLuminance = 720.0f;
    display.maxFullFrameLuminance = 380.0f;
    display.minLuminance = 0.01f;
    display.exposureEv = -0.5f;
    display.saturation = 1.2f;
    display.sdrWhite = 0.95f;
    display.sdrBlack = 0.03f;
    display.uiWhiteNits = 300.0f;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_display_calibration_test.json";
    std::string error;
    Require(SaveEngineSettings(path, saved, error), ("the settings save: " + error).c_str());
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), ("the settings load: " + error).c_str());
    std::filesystem::remove(path);
    Require(loaded.view.renderDebug.display == display, "the calibration comes back");
}

// The Preferences window's master volume and mute survive the settings file; a file from before
// they existed plays at full volume.
void TestAudioSettingsSurviveTheSettingsFile()
{
    EngineSettings saved;
    saved.audio.masterVolume = 0.35f;
    saved.audio.muted = true;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_audio_settings_test.json";
    std::string error;
    Require(SaveEngineSettings(path, saved, error), ("the settings save: " + error).c_str());
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), ("the settings load: " + error).c_str());
    Require(loaded.audio == saved.audio, "the volume and mute come back");
    Require(loaded.audio.EffectiveVolume() == 0.0f, "muted is silent whatever the volume");

    std::ofstream(path) << "{ \"version\": 1 }";
    Require(LoadEngineSettings(path, loaded, error), ("the old settings load: " + error).c_str());
    std::filesystem::remove(path);
    Require(loaded.audio.masterVolume == 1.0f && !loaded.audio.muted, "without audio settings the volume is full");
}

// The Preferences window's priority and CPUs survive the settings file; a file from before they existed
// runs at high priority on every CPU, and a value this build does not know keeps the default.
void TestProcessSettingsSurviveTheSettingsFile()
{
    EngineSettings saved;
    saved.process.priority = platform::process::ProcessPriority::AboveNormal;
    saved.process.cpus = platform::process::CpuSelection::Custom;
    saved.process.customCpus = {0, 2, 3, 17};
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_process_settings_test.json";
    std::string error;
    Require(SaveEngineSettings(path, saved, error), ("the settings save: " + error).c_str());
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), ("the settings load: " + error).c_str());
    Require(loaded.process == saved.process, "the priority and CPUs come back");

    std::ofstream(path) << "{ \"version\": 1 }";
    Require(LoadEngineSettings(path, loaded, error), ("the old settings load: " + error).c_str());
    Require(loaded.process == platform::process::ProcessAllocation{}, "without process settings the defaults apply");
    Require(loaded.process.priority == platform::process::ProcessPriority::High, "the default priority is high");

    std::ofstream(path, std::ios::trunc) << R"({"version": 1, "process": {"priority": "realtime", "cpus": "performance"}})";
    Require(LoadEngineSettings(path, loaded, error), ("the odd settings load: " + error).c_str());
    std::filesystem::remove(path);
    Require(loaded.process.priority == platform::process::ProcessPriority::High, "an unknown priority keeps high");
    Require(loaded.process.cpus == platform::process::CpuSelection::Performance, "a known selection still reads");
}

// Only the theme colours the user changed are saved and come back; a file from before the theme had a
// version holds the whole palette it was saved with, and is not read, so the built-in palette applies.
void TestThemeKeepsOnlyChangedColours()
{
    EngineSettings saved;
    saved.editorUi.theme.hasCustomColors = true;
    saved.editorUi.theme.colors[ImGuiCol_Text] = ImVec4(0.25f, 0.5f, 0.75f, 1.0f);
    saved.editorUi.theme.colorDefined[ImGuiCol_Text] = true;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_theme_test.json";
    std::string error;
    Require(SaveEngineSettings(path, saved, error), ("the settings save: " + error).c_str());
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), ("the settings load: " + error).c_str());
    Require(loaded.editorUi.theme.hasCustomColors, "a changed colour comes back");
    Require(loaded.editorUi.theme.colorDefined[ImGuiCol_Text] && loaded.editorUi.theme.colors[ImGuiCol_Text].y == 0.5f,
            "the changed colour keeps its value");
    Require(!loaded.editorUi.theme.colorDefined[ImGuiCol_WindowBg], "an unchanged colour is not saved");

    {
        std::ofstream legacy(path, std::ios::trunc);
        legacy << R"({"version": 1, "ui": {"theme": {"colors": {"WindowBg": [0.039, 0.039, 0.039, 1.0]}}}})";
    }
    EngineSettings legacyLoaded;
    Require(LoadEngineSettings(path, legacyLoaded, error), ("the legacy settings load: " + error).c_str());
    std::filesystem::remove(path);
    Require(!legacyLoaded.editorUi.theme.hasCustomColors, "a whole-palette theme without a version is ignored");
}

// The camera's and the renderer's settings survive the settings file, field for field.
void TestViewSettingsSurviveTheSettingsFile()
{
    Camera camera;
    camera.fovDegrees = 62.5f;
    camera.farPlane = 4321.0f;
    camera.mouseSensitivity = 0.37f;
    camera.autoExposure.enabled = false;
    camera.exposureEv100 = 11.25f;
    camera.autoExposure.compensationEv = -0.7f;
    camera.autoWhiteBalance.enabled = false;
    camera.autoWhiteBalance.degree = 0.35f;
    camera.autoWhiteBalance.targetKelvin = 5600.0f;
    RenderDebugSettings renderDebug;
    renderDebug.gbufferView = GBufferDebugView::Normal;
    renderDebug.toneMapper = ToneMapper::PbrNeutral;
    renderDebug.taa = false;
    renderDebug.shadowDistance = 150.0f;
    renderDebug.renderScale = 0.75f;
    renderDebug.ao.sliceCount = 3;
    renderDebug.ddgi.hysteresis = 0.9f;
    renderDebug.bloom.strength = 0.123456789f;

    EngineSettings saved;
    Require(UpdateEngineViewSettings(saved.view, camera, renderDebug), "changed settings mark the view changed");
    Require(!UpdateEngineViewSettings(saved.view, camera, renderDebug), "the same settings again change nothing");
    Require(saved.view.renderDebug.gbufferView == GBufferDebugView::Off, "the G-buffer view is not saved");

    // While auto exposure runs, the exposure it writes is not a setting.
    Camera autoCamera = camera;
    autoCamera.autoExposure.enabled = true;
    autoCamera.exposureEv100 = 3.0f;
    EngineViewSettings autoView = saved.view;
    UpdateEngineViewSettings(autoView, autoCamera, renderDebug);
    Require(autoView.exposureEv100 == 11.25f, "auto exposure keeps the manual exposure");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_view_settings_test.json";
    std::string error;
    Require(SaveEngineSettings(path, saved, error), ("the settings save: " + error).c_str());
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), ("the settings load: " + error).c_str());
    std::filesystem::remove(path);
    Require(loaded.view == saved.view, "every camera and render setting comes back");

    Camera applied;
    RenderDebugSettings appliedDebug;
    appliedDebug.gbufferView = GBufferDebugView::GeometricNormal;
    ApplyEngineViewSettings(loaded.view, applied, appliedDebug);
    Require(applied.fovDegrees == 62.5f && applied.autoWhiteBalance.targetKelvin == 5600.0f, "the camera takes the settings");
    Require(appliedDebug.toneMapper == ToneMapper::PbrNeutral && !appliedDebug.taa, "the renderer takes the settings");
    Require(appliedDebug.gbufferView == GBufferDebugView::GeometricNormal, "applying leaves the G-buffer view alone");
}

void TestRegistration()
{
    CommandRegistry registry;
    Require(registry.Register(Command{.id = "render.reload_shaders", .label = "Reload Shaders", .menuPath = "Render/Reload Shaders"}), "a command registers");
    Require(!registry.Register(Command{.id = "render.reload_shaders", .label = "Again", .menuPath = "Render/Again"}), "a taken id is refused");
    Require(!registry.Register(Command{.id = "", .label = "No Id", .menuPath = "Render/No Id"}), "an empty id is refused");
    Require(!registry.Register(Command{.id = "top", .label = "Top", .menuPath = "Top"}), "an item needs a menu");
    Require(!registry.Register(Command{.id = "deep", .label = "Deep", .menuPath = "A/B/C/Deep"}), "only one submenu level is allowed");
    Require(!registry.Register(Command{.id = "gap", .label = "Gap", .menuPath = "A//Gap"}), "empty path segments are refused");
    Require(registry.Register(Command{.id = "palette.only", .label = "Palette Only", .menuPath = ""}), "a command can be in no menu");

    Require(registry.GetCommands().size() == 2, "refused commands are not kept");
    Require(registry.Find("render.reload_shaders") != nullptr, "a command is found by id");
    Require(registry.Find("missing") == nullptr, "an unknown id is not found");

    const CommandMenuNode& root = registry.GetMenuRoot();
    Require(root.children.size() == 1, "a command with no menuPath adds no menu");
    Require(root.children[0].label == "Render", "the menu is named by the path");
    Require(root.children[0].children[0].label == "Reload Shaders", "the item is the last path segment");
}

void TestMenuTree()
{
    CommandRegistry registry;
    registry.Register(Command{.id = "file.open", .label = "Open", .menuPath = "File/Open..."});
    registry.Register(Command{.id = "render.mode.a", .label = "A", .menuPath = "Render/Mode/A"});
    registry.AddSeparator("File");
    registry.Register(Command{.id = "file.exit", .label = "Exit", .menuPath = "File/Exit"});
    registry.Register(Command{.id = "render.mode.b", .label = "B", .menuPath = "Render/Mode/B"});
    Require(!registry.AddSeparator("A/B/C"), "a separator deeper than a submenu is refused");

    const CommandMenuNode& root = registry.GetMenuRoot();
    Require(root.children.size() == 2 && root.children[0].label == "File" && root.children[1].label == "Render", "menus keep the order they first appear in");

    const CommandMenuNode* file = FindMenu(root, "File");
    Require(file->children.size() == 3, "File has open, a separator and exit");
    Require(file->children[1].kind == CommandMenuNode::Kind::Separator, "the separator sits where it was added");
    Require(registry.GetCommands()[file->children[2].commandIndex].id == "file.exit", "an item points at its command");

    const CommandMenuNode* mode = FindMenu(*FindMenu(root, "Render"), "Mode");
    Require(mode != nullptr && mode->children.size() == 2, "items with the same submenu share it");
}

void TestExecute()
{
    CommandRegistry registry;
    int runs = 0;
    bool enabled = false;
    bool checked = true;
    registry.Register(Command{
        .id = "test.run",
        .label = "Run",
        .execute =
            [&runs]
        {
            ++runs;
        },
        .isChecked =
            [&checked]
        {
            return checked;
        },
        .isEnabled =
            [&enabled]
        {
            return enabled;
        }});

    Require(!Run(registry, "test.run") && runs == 0, "a disabled command does not run");
    enabled = true;
    Require(Run(registry, "test.run") && runs == 1, "an enabled command runs");
    Require(!Run(registry, "missing"), "an unknown id does not run");

    const Command& command = *registry.Find("test.run");
    Require(IsCommandChecked(command), "isChecked reports the command's state");
    Require(!IsCommandChecked(Command{.id = "plain"}) && IsCommandEnabled(Command{.id = "plain"}), "commands are enabled and unchecked by default");
}

void TestFormatShortcut()
{
    Require(FormatShortcut(0).empty(), "no shortcut formats as nothing");
    Require(FormatShortcut(ImGuiMod_Ctrl | ImGuiKey_R) == "Ctrl+R", "Ctrl+R");
    Require(FormatShortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P) == "Ctrl+Shift+P", "Ctrl+Shift+P");
    Require(FormatShortcut(ImGuiKey_F5) == "F5", "F5");
}

void TestCommandPalette()
{
    Require(FuzzyMatchScore("", "Anything") == 0, "an empty query matches everything");
    Require(FuzzyMatchScore("svas", "Save Scene As").has_value(), "a query matches characters in order");
    Require(!FuzzyMatchScore("sa", "As").has_value(), "but not out of order");
    Require(FuzzyMatchScore("SAVE", "save scene").has_value(), "ignoring case");
    Require(*FuzzyMatchScore("save", "Save Scene") > *FuzzyMatchScore("save", "Snap Above Vertices"), "consecutive matches score higher");
    Require(*FuzzyMatchScore("ss", "Save Scene") > *FuzzyMatchScore("ss", "Sassy"), "word starts score higher");

    CommandRegistry registry;
    bool enabled = false;
    registry.Register(Command{.id = "file.open", .label = "Open Scene", .menuPath = "File/Open Scene..."});
    registry.Register(Command{.id = "file.save", .label = "Save Scene", .menuPath = "File/Save Scene"});
    registry.Register(Command{
        .id = "edit.undo",
        .label = "Undo",
        .menuPath = "Edit/Undo",
        .isEnabled = [&enabled]
        {
            return enabled;
        }});
    Require(FindPaletteCommands(registry, "").size() == 2, "the palette lists only enabled commands");
    const std::vector<std::size_t> save = FindPaletteCommands(registry, "save");
    Require(save.size() == 1 && registry.GetCommands()[save[0]].id == "file.save", "a query narrows the list");
    Require(FindPaletteCommands(registry, "file").size() == 2, "the menu path matches too");
    enabled = true;
    Require(FindPaletteCommands(registry, "undo").size() == 1, "an enabled command is listed");
}

void TestEditorCommands()
{
    CommandRegistry registry;
    EditorCommandState state;
    bool sceneVisible = false;
    bool themeVisible = true;
    int resets = 0;
    const std::array<EditorPanelMenuEntry, 2> panels = {{
        {"scene", "Scene", "", &sceneVisible},
        {"theme", "Theme", "", &themeVisible},
    }};
    EditorWindowCommands window;
    window.panels = panels;
    window.resetLayout = [&resets]
    {
        ++resets;
    };
    int opens = 0;
    int saves = 0;
    int deletes = 0;
    bool hasSelection = false;
    std::vector<LightType> createdLights;
    EditorSceneCommands scene;
    scene.openScene = [&opens]
    {
        ++opens;
    };
    scene.saveScene = [&saves]
    {
        ++saves;
    };
    scene.deleteSelection = [&deletes]
    {
        ++deletes;
    };
    scene.hasSelection = [&hasSelection]
    {
        return hasSelection;
    };
    scene.createLight = [&createdLights](LightType type)
    {
        createdLights.push_back(type);
    };
    int steps = 0;
    scene.stepSimulation = [&steps]
    {
        ++steps;
    };
    int frames = 0;
    bool canFrame = false;
    scene.frameSelection = [&frames]
    {
        ++frames;
    };
    scene.canFrameSelection = [&canFrame]
    {
        return canFrame;
    };
    int recordingToggles = 0;
    scene.toggleVideoRecording = [&recordingToggles]
    {
        ++recordingToggles;
    };
    RegisterEditorCommands(registry, state, window, scene);

    const CommandMenuNode& root = registry.GetMenuRoot();
    const std::vector<std::string> expectedMenus = {"File", "Edit", "Scene", "View", "Render", "Tools", "Window", "Help"};
    Require(root.children.size() == expectedMenus.size(), "eight top-level menus");
    for (std::size_t index = 0; index < expectedMenus.size(); ++index)
    {
        Require(root.children[index].label == expectedMenus[index], "top-level menus are in order");
    }

    std::vector<const Command*> items;
    int maxDepth = 0;
    CollectItems(registry, root, 0, maxDepth, items);
    Require(maxDepth <= 2, "at most one submenu level");

    std::set<ImGuiKeyChord> shortcuts;
    for (const Command& command : registry.GetCommands())
    {
        if (command.shortcut != 0)
        {
            Require(shortcuts.insert(command.shortcut).second, "no two commands share a shortcut");
        }
    }
    Require(registry.Find("render.reload_shaders")->shortcut == (ImGuiMod_Ctrl | ImGuiKey_R), "Reload Shaders is Ctrl+R");
    Require(registry.Find("tools.command_palette")->shortcut == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P), "the command palette is Ctrl+Shift+P");

    const Command& fullscreen = *registry.Find("view.viewport_fullscreen");
    Require(fullscreen.shortcut == ImGuiKey_F11, "the fullscreen viewport is F11");
    Require(!IsCommandChecked(fullscreen) && !state.viewportFullscreen, "the viewport starts windowed");
    Require(Run(registry, "view.viewport_fullscreen") && state.viewportFullscreen && IsCommandChecked(fullscreen), "the command turns fullscreen on");
    Require(Run(registry, "view.viewport_fullscreen") && !state.viewportFullscreen, "and off again");

    // F points the viewport camera at the selection, from any panel, while there is one to frame.
    const Command& frame = *registry.Find("view.frame_selected");
    Require(frame.shortcut == ImGuiKey_F, "Frame Selected is F");
    Require(!Run(registry, "view.frame_selected") && frames == 0, "nothing to frame without a selection");
    canFrame = true;
    Require(Run(registry, "view.frame_selected") && frames == 1, "the command frames the selection");

    // Record Viewport toggles a recording the backend runs; it shows checked while one does.
    const Command& record = *registry.Find("tools.record_viewport");
    Require(record.shortcut == (ImGuiMod_Shift | ImGuiKey_F12), "Record Viewport is Shift+F12");
    Require(record.menuPath == "Tools/Record Viewport", "Record Viewport is in the Tools menu");
    Require(!IsCommandChecked(record), "nothing is recorded at first");
    Require(Run(registry, "tools.record_viewport") && recordingToggles == 1, "the command asks for a recording");
    state.videoRecording = true;
    Require(IsCommandChecked(record), "the command is checked while recording");
    Require(Run(registry, "tools.record_viewport") && recordingToggles == 2, "and stops it");
    state.videoRecording = false;
    {
        CommandRegistry unbound;
        EditorCommandState unboundState;
        RegisterEditorCommands(unbound, unboundState, window);
        Require(!IsCommandEnabled(*unbound.Find("tools.record_viewport")), "without a backend to record, the command is disabled");
    }

    // View is debug visualization, Render is the pipeline: neither holds the other's commands.
    for (const Command* command : items)
    {
        if (command->id.starts_with("view."))
        {
            Require(command->menuPath.starts_with("View/"), "view commands are in View");
        }
        if (command->id.starts_with("render."))
        {
            Require(command->menuPath.starts_with("Render/"), "render commands are in Render");
        }
    }

    // The renderer has no hardware ray tracing yet: the pipelines that need it are disabled.
    Require(!state.rayTracingSupported, "ray tracing starts unsupported");
    Require(!Run(registry, "render.pipeline.hybrid") && !Run(registry, "render.ray_tracing"), "no hybrid pipeline or ray tracing without support");
    Require(!Run(registry, "view.wireframe") && !state.wireframe, "no wireframe without line pipelines");

    // Radio groups hold one choice.
    state.rayTracingSupported = true;
    Run(registry, "render.pipeline.hybrid");
    Require(IsCommandChecked(*registry.Find("render.pipeline.hybrid")), "the chosen pipeline is checked");
    Require(!IsCommandChecked(*registry.Find("render.pipeline.rasterization")), "the other pipelines are not");
    Run(registry, "render.pipeline.path_tracing");
    Require(IsCommandChecked(*registry.Find("render.ray_tracing")), "path tracing shows ray tracing on");
    Require(!IsCommandEnabled(*registry.Find("render.ray_tracing")), "and it cannot be turned off there");

    state.rayTracingSupported = false;
    Require(!IsCommandEnabled(*registry.Find("render.pipeline.path_tracing")), "no path tracing without ray tracing");

    // Every view the tone mapping pass has is a View command, so the toolbar always shows the
    // current one.
    for (uint32_t view = 0; view <= static_cast<uint32_t>(GBufferDebugView::DdgiProbes); ++view)
    {
        const std::string id = DebugViewCommandId(static_cast<GBufferDebugView>(view));
        Require(!id.empty() && registry.Find(id) != nullptr, "every debug view has a command");
    }
    Require(Run(registry, "view.debug.emissive") && state.debugView == GBufferDebugView::Emissive, "a debug view command picks its view");
    Require(IsCommandChecked(*registry.Find("view.debug.emissive")) && !IsCommandChecked(*registry.Find("view.debug.lit")), "and only it is checked");
    state.gbufferAvailable = false;
    Require(!IsCommandEnabled(*registry.Find("view.debug.albedo")) && IsCommandEnabled(*registry.Find("view.debug.lit")), "without a G-buffer only the lit view is offered");
    state.gbufferAvailable = true;

    // Tone mapping is the renderer's, except under the Khronos reference view.
    Require(Run(registry, "render.tone_mapping.none") && state.toneMapping == ToneMapper::None, "tone mapping commands pick the operator");
    state.khronosReference = true;
    Require(!Run(registry, "render.tone_mapping.gt7") && state.toneMapping == ToneMapper::None, "the Khronos reference view keeps its own");
    state.khronosReference = false;

    Require(Run(registry, "tool.rotate") && state.transformTool == TransformTool::Rotate, "the rotate tool is a choice of its own");
    Require(Run(registry, "view.gizmos") && !state.gizmos, "gizmos toggle off");
    Require(state.minimap && Run(registry, "view.minimap") && !state.minimap, "the minimap starts on and toggles off");
    Require(state.viewportUi && Run(registry, "view.viewport_ui") && !state.viewportUi, "the viewport UI starts on and toggles off");

    // Play controls.
    Require(!IsCommandEnabled(*registry.Find("scene.step")), "step needs a paused simulation");
    Run(registry, "scene.play");
    Run(registry, "scene.pause");
    Require(IsCommandChecked(*registry.Find("scene.pause")) && Run(registry, "scene.step") && steps == 1, "paused: pause is on and step runs");

    // The scene commands reach the functions they were given; the others do nothing.
    Run(registry, "file.open_scene");
    Run(registry, "file.save_scene");
    Require(opens == 1 && saves == 1, "open and save reach the controller");
    Require(!Run(registry, "file.save_scene_as"), "a command with no function is disabled");
    Require(!IsCommandEnabled(*registry.Find("edit.undo")) && !IsCommandEnabled(*registry.Find("render.reload_shaders")), "so are the ones nothing implements yet");
    Require(!IsCommandEnabled(*registry.Find("edit.delete")), "delete needs a selection");
    hasSelection = true;
    Require(Run(registry, "edit.delete") && deletes == 1, "delete runs with a selection");
    Run(registry, "scene.create_light.spot");
    Run(registry, "scene.create_light.area");
    Require(createdLights == std::vector<LightType>{LightType::Spot, LightType::Area}, "each create light command names its type");

    // The Window menu is the panels it was given.
    const CommandMenuNode* windowMenu = FindMenu(root, "Window");
    Require(windowMenu->children[0].label == "Scene" && windowMenu->children[1].label == "Theme", "the Window menu lists the panels in order");
    Run(registry, "window.scene");
    Run(registry, "window.theme");
    Require(sceneVisible && !themeVisible, "panel commands toggle their panels");
    Run(registry, "window.show_all");
    Require(sceneVisible && themeVisible, "show all opens every panel");
    Run(registry, "window.reset_layout");
    Require(resets == 1, "reset layout reaches the controller");

    // The toolbar only names registered commands.
    const ToolbarLayout layout = BuildEditorToolbarLayout();
    for (const std::vector<ToolbarGroup>* section : {&layout.left, &layout.center, &layout.right})
    {
        for (const ToolbarGroup& group : *section)
        {
            for (const ToolbarItem& item : group)
            {
                Require(item.commandId.empty() || registry.Find(item.commandId) != nullptr, "toolbar buttons name commands");
                for (const std::string& id : item.dropdownCommandIds)
                {
                    Require(registry.Find(id) != nullptr, "toolbar dropdowns name commands");
                }
            }
        }
    }
}
}

int main()
{
    ImGui::CreateContext();
    try
    {
        TestRegistration();
        TestMenuTree();
        TestExecute();
        TestFormatShortcut();
        TestCommandPalette();
        TestEditorCommands();
        TestWindowStatesSurviveTheSettingsFile();
        TestThemeKeepsOnlyChangedColours();
        TestViewSettingsSurviveTheSettingsFile();
        TestAudioSettingsSurviveTheSettingsFile();
        TestDisplayCalibrationSurvivesTheSettingsFile();
        TestProcessSettingsSurviveTheSettingsFile();
    }
    catch (const std::exception& error)
    {
        std::cerr << "command_registry_tests failed: " << error.what() << '\n';
        ImGui::DestroyContext();
        return 1;
    }
    ImGui::DestroyContext();
    std::cout << "command_registry_tests passed\n";
    return 0;
}
