#include "editor_ui.h"
#include "ui/editor_menu_toolbar.h"
#include "ui/editor_ui_internal.h"
#include "ui/modals/about_modal.h"
#include "ui/modals/import_conflict_modal.h"
#include "ui/modals/kn5_import_modal.h"
#include "ui/modals/scene_reset_modal.h"
#include "ui/modals/unsaved_changes_modal.h"
#include "ui/panels/asset_browser_panel.h"
#include "ui/panels/camera_panel.h"
#include "ui/panels/drive_paths_panel.h"
#include "ui/panels/graphics_debug_panel.h"
#include "ui/panels/photo_mode_panel.h"
#include "ui/panels/quad_recording_panel.h"
#include "ui/panels/input_monitor_panel.h"
#include "ui/panels/scene_panel.h"
#include "ui/panels/suspension_rigs_panel.h"
#include "ui/panels/theme_panel.h"
#include "ui/panels/vehicle_panel.h"
#include "ui/panels/viewport_panel.h"
#include "ui/windows/keyboard_shortcuts_window.h"
#include "ui/windows/model_processor_window.h"
#include "ui/windows/preferences_window.h"

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/logic/editor_world.h>
#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <utility>

namespace me
{

namespace
{
// Window > Auto Layout, when it is on and the viewport renders at a fixed size: the backend's
// (--viewport-size, a recording), else the viewport resolution setting. The render scale is left out:
// the picture is shown at the resolution's size whatever share of it is rendered.
std::optional<ViewportAutoLayout> BuildViewportAutoLayout(const EditorSharedState& state, const char* viewportWindowName, float uiScale)
{
    if (!state.commands.autoLayout || !state.viewportPanelArea.has_value())
    {
        return std::nullopt;
    }
    ImVec2 pixels;
    if (state.forcedViewportExtent.has_value() && state.forcedViewportExtent->IsValid())
    {
        pixels = ImVec2(static_cast<float>(state.forcedViewportExtent->width), static_cast<float>(state.forcedViewportExtent->height));
    }
    else if (state.renderDebug.viewportResolution.fixed)
    {
        const ViewportResolutionSettings& resolution = state.renderDebug.viewportResolution;
        pixels = ImVec2(
            static_cast<float>(std::clamp(resolution.width, ViewportResolutionSettings::kMinSize, ViewportResolutionSettings::kMaxSize)),
            static_cast<float>(std::clamp(resolution.height, ViewportResolutionSettings::kMinSize, ViewportResolutionSettings::kMaxSize)));
    }
    else
    {
        return std::nullopt;
    }
    // Display pixels per point: 2 on Retina, 1 on Windows, where the UI scale enlarges the fonts instead.
    const float pixelsPerPoint = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
    return ViewportAutoLayout{
        viewportWindowName, *state.viewportPanelArea, ImVec2(pixels.x / pixelsPerPoint, pixels.y / pixelsPerPoint), uiScale};
}
}

EditorUiController::EditorUiController()
{
    RegisterWindows();
    RegisterCommands();
}

void EditorUiController::RegisterWindows()
{
    // Dockable panels, in the Window menu's order. A new panel needs its class and a line here.
    m_windows.Register<ScenePanel>();
    m_windows.Register<ViewportPanel>();
    m_windows.Register<CameraPanel>();
    m_windows.Register<GraphicsDebugPanel>();
    m_windows.Register<AssetBrowserPanel>();
    m_windows.Register<InputMonitorPanel>();
    m_windows.Register<VehiclePanel>();
    m_windows.Register<SuspensionRigsPanel>();
    m_windows.Register<DrivePathsPanel>();
    m_windows.Register<QuadRecordingPanel>();
    m_windows.Register<PhotoModePanel>();
    m_windows.Register<ThemePanel>();
    // Floating tool windows, opened by commands or by other windows.
    m_windows.Register<ModelProcessorWindow>();
    m_windows.Register<PreferencesWindow>();
    m_windows.Register<KeyboardShortcutsWindow>();
    // Modals, drawn last so they are over everything else.
    m_windows.Register<SceneResetModal>();
    m_windows.Register<UnsavedChangesModal>();
    m_windows.Register<Kn5ImportModal>();
    m_windows.Register<ImportConflictModal>();
    m_windows.Register<AboutModal>();
}

void EditorUiController::RegisterCommands()
{
    // The Window menu lists the panels in the order they were registered.
    const std::vector<EditorPanelMenuEntry> panels = m_windows.BuildPanelMenuEntries();
    EditorWindowCommands window;
    window.panels = panels;
    window.resetLayout = [this]
    {
        m_resetDockLayoutRequested = true;
    };

    EditorSceneCommands scene;
    scene.newScene = [this]
    {
        // With unsaved changes the question is what becomes of them; without, whether to start again.
        if (m_state.sceneUnsaved)
        {
            m_windows.Get<UnsavedChangesModal>().Ask(UnsavedChangesModal::Then::NewScene);
        }
        else
        {
            m_windows.Get<SceneResetModal>().Ask(SceneResetModal::Reset::New);
        }
    };
    scene.clearScene = [this]
    {
        m_windows.Get<SceneResetModal>().Ask(SceneResetModal::Reset::Clear);
    };
    scene.openScene = [this]
    {
        m_openSceneRequested = true;
    };
    scene.saveScene = [this]
    {
        m_saveSceneRequested = true;
    };
    scene.saveSceneAs = [this]
    {
        m_saveSceneAsRequested = true;
    };
    scene.importModel = [this]
    {
        m_importModelRequested = true;
    };
    // The same way out as closing the window.
    scene.exit = []
    {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    };
    scene.deleteSelection = [this]
    {
        m_commandActions.deleteSelectedSceneEntity = true;
    };
    scene.hasSelection = [this]
    {
        return m_hasSceneSelection;
    };
    scene.frameSelection = [this]
    {
        m_commandActions.frameSelectedSceneEntity = true;
    };
    // While a car is driven the camera is the driver's (or chases the car).
    scene.canFrameSelection = [this]
    {
        return m_hasSceneSelection && !m_state.vehicleStatus.active;
    };
    scene.createEntity = [this]
    {
        m_commandActions.createSceneEntity = true;
    };
    // Named as the Scene panel's Add Light names them.
    scene.createLight = [this](LightType type)
    {
        m_commandActions.createLightEntity = EditorUiActions::LightCreate{std::string(GetLightTypeLabel(type)) + " Light", type};
    };
    scene.captureViewport = [this]
    {
        m_commandActions.captureViewport = true;
    };
    scene.toggleVideoRecording = [this]
    {
        m_commandActions.toggleVideoRecording = true;
    };
    scene.toggleQuadRecording = [this]
    {
        m_commandActions.toggleQuadRecording = true;
    };
    scene.takePhoto = [this]
    {
        m_commandActions.takePhoto = true;
    };
    scene.stepSimulation = [this]
    {
        m_commandActions.stepVehicleDrive = true;
    };
    scene.openPreferences = [this]
    {
        m_windows.Open<PreferencesWindow>();
    };
    // The scene's environment, fog and clouds are edited in the Scene panel.
    scene.openSceneSettings = [this]
    {
        m_windows.Open<ScenePanel>();
    };
    scene.showDocumentation = [this]
    {
        OpenDocumentation();
    };
    scene.showKeyboardShortcuts = [this]
    {
        m_windows.Open<KeyboardShortcutsWindow>();
    };
    scene.showAbout = [this]
    {
        m_windows.Get<AboutModal>().Open();
    };
    RegisterEditorCommands(m_commands, m_state.commands, window, scene);
    m_toolbarLayout = BuildEditorToolbarLayout();
}

void EditorUiController::BeginFrame(SDL_Window* window, const EngineSettings& settings)
{
    m_window = window;
    m_style.BeginFrame(window);
    if (!m_hasAppliedEngineSettings)
    {
        m_style.ApplySettings(settings.editorUi);
        m_state.audio = settings.audio;
        m_state.commands.graphicsBackend = settings.graphics.backend;
        m_state.process = settings.process;
        m_state.quadRecording = settings.quadRecording;
        m_state.photoMode = settings.photoMode;
        m_windows.ApplyOpenState(settings.editorUi.windows);
        m_state.commands.autoLayout = settings.editorUi.autoLayout;
        m_hasAppliedEngineSettings = true;
    }

    m_style.ApplyUiScale();
    ImGuizmo::BeginFrame();
}

void EditorUiController::WriteEngineSettings(EngineSettings& settings) const
{
    settings.version = 1;
    settings.audio = m_state.audio;
    settings.graphics.backend = m_state.commands.graphicsBackend;
    settings.process = m_state.process;
    settings.quadRecording = m_state.quadRecording;
    settings.photoMode = m_state.photoMode;
    m_windows.WriteOpenState(settings.editorUi.windows);
    settings.editorUi.autoLayout = m_state.commands.autoLayout;
    m_style.WriteSettings(settings.editorUi);
}

void EditorUiController::RequestAssetBrowserRefresh()
{
    m_windows.Get<AssetBrowserPanel>().Refresh();
}

void EditorUiController::QueueDroppedFile(std::string path)
{
    m_windows.Get<AssetBrowserPanel>().QueueDroppedFile(std::move(path));
}

EditorUiFrameResult EditorUiController::Draw(
    Camera& camera,
    ViewportMatrices& matrices,
    IEditorWorld& scene,
    const std::string& currentModelPath,
    const std::string& lastLoadError,
    const std::string& lastSceneIoError,
    const std::string& sceneUploadStatus,
    ImTextureID viewportTextureId,
    RenderExtent viewportExtent,
    RenderBackendType currentBackendType)
{
    static_cast<void>(currentModelPath);

    EditorUiFrameResult result{};
    result.viewportExtent = viewportExtent;
    const EditorFrameInput frame{
        lastLoadError,
        lastSceneIoError,
        sceneUploadStatus,
        viewportTextureId,
        viewportExtent,
        currentBackendType};
    EditorContext context{scene, camera, matrices, frame, result, m_state, m_style, m_windows, m_commands};

    const float previousUiScale = m_style.UiScaleMultiplier();
    const EngineAudioSettings previousAudio = m_state.audio;
    const RenderBackendType previousGraphicsBackend = m_state.commands.graphicsBackend;
    const platform::process::ProcessAllocation previousProcess = m_state.process;
    const QuadRecordingSettings previousQuadRecording = m_state.quadRecording;
    const PhotoModeSettings previousPhotoMode = m_state.photoMode;
    // The Quad Recording window asks for its preview again each frame it draws.
    m_state.quadRecordingPreview = false;
    // Every panel's open state, to save the settings when one opens or closes.
    const std::vector<bool> previousOpen = m_windows.CapturePanelOpenState();
    const bool previousAutoLayout = m_state.commands.autoLayout;

    // Shortcuts first, so what they change shows in this frame's menus and panels. The menu bar
    // and the toolbar come before the dock space, which fills the area they leave.
    m_hasSceneSelection = scene.HasSelection();
    SyncCommandStateFromEditor(scene);
    const EditorCommandState commandStateBefore = m_state.commands;
    ProcessCommandShortcuts(m_commands);
    // Escape leaves the fullscreen viewport as F11 does: with no menu there is nothing else to click.
    // An open popup (a typed-path prompt, the command palette) takes Escape to close itself instead.
    if (m_state.commands.viewportFullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        m_state.commands.viewportFullscreen = false;
    }
    UpdateWindowFullscreen();
    const bool fullscreen = m_state.commands.viewportFullscreen;
    if (!fullscreen)
    {
        DrawMainMenu(m_commands);
        DrawToolbar(m_commands, m_toolbarLayout, m_style.EffectiveUiScale());
    }
    // Before the command state is applied, so what a command picked here shows this frame.
    if (std::exchange(m_state.commands.commandPaletteRequested, false))
    {
        m_commandPalette.Open();
    }
    m_commandPalette.Draw(m_commands, m_style.EffectiveUiScale());
    ApplyCommandStateToEditor(commandStateBefore, scene);
    if (!fullscreen)
    {
        DrawEditorDockspace(
            std::exchange(m_resetDockLayoutRequested, false),
            m_windows.GetPanels(),
            BuildViewportAutoLayout(m_state, m_windows.Get<ViewportPanel>().GetTitle().c_str(), m_style.EffectiveUiScale()),
            m_autoLayoutKey);
    }
    result.actions = std::exchange(m_commandActions, {});
    HandleFileCommands(context);

    // Every window, panel and modal. Over the fullscreen viewport only those that draw there.
    m_windows.TickAndDraw(context, fullscreen);
    // Another scene asked for (File > Open, the Scene panel, the asset browser) while this one has
    // unsaved changes: asked about them first.
    if (result.actions.selectedSceneLoadPath.has_value() && m_state.sceneUnsaved && !result.actions.discardUnsavedChanges)
    {
        m_windows.Get<UnsavedChangesModal>().Ask(UnsavedChangesModal::Then::OpenScene, *result.actions.selectedSceneLoadPath);
        result.actions.selectedSceneLoadPath.reset();
    }

    const bool windowToggled = previousOpen != m_windows.CapturePanelOpenState() || previousAutoLayout != m_state.commands.autoLayout;
    // The Theme panel sets engineSettingsChanged itself when the palette changes.
    result.engineSettingsChanged = result.engineSettingsChanged ||
                                   std::abs(previousUiScale - m_style.UiScaleMultiplier()) > 0.0001f ||
                                   windowToggled || previousAudio != m_state.audio || previousGraphicsBackend != m_state.commands.graphicsBackend ||
                                   previousProcess != m_state.process ||
                                   previousQuadRecording != m_state.quadRecording || previousPhotoMode != m_state.photoMode;

    result.renderDebug = m_state.renderDebug;
    result.audio = m_state.audio;
    if (previousProcess != m_state.process)
    {
        result.processAllocation = m_state.process;
    }
    result.vehicleTuning = m_state.vehicle.tuning;
    result.vehicleCamera = m_state.vehicle.camera;
    result.vehicleHaptics = m_state.vehicle.haptics;
    result.vehicleSteeringAssist = m_state.vehicle.steeringAssist;
    result.vehicleManualGearbox = m_state.vehicle.manualGearbox;
    result.vehiclePhysicsStepSeconds = 1.0f / static_cast<float>(std::max(m_state.vehicle.physicsRateHz, 1));
    result.quadRecording = m_state.quadRecording;
    result.quadRecordingPreview = m_state.quadRecordingPreview;
    result.photoMode = m_state.photoMode;
    return result;
}

void EditorUiController::UpdateWindowFullscreen()
{
    if (m_state.commands.viewportFullscreen == m_windowFullscreen)
    {
        return;
    }
    m_windowFullscreen = m_state.commands.viewportFullscreen;
    m_state.fullscreenEnteredTime = ImGui::GetTime();
    // SDL's fullscreen without a display mode is the borderless one at the desktop's resolution.
    if (m_window != nullptr && !SDL_SetWindowFullscreen(m_window, m_windowFullscreen))
    {
        LOG_WARN("Could not switch the window to {}: {}", m_windowFullscreen ? "fullscreen" : "windowed", SDL_GetError());
    }
}

void EditorUiController::SyncCommandStateFromEditor(const IEditorWorld& scene)
{
    const ImGuizmo::OPERATION operation = scene.GetGizmoSettings().operation;
    m_state.commands.transformTool = operation == ImGuizmo::SCALE    ? TransformTool::Scale
                                   : operation == ImGuizmo::ROTATE ? TransformTool::Rotate
                                                                   : TransformTool::Move;
    // Every debug view has a View command, so the menu and the toolbar show whichever is set, also
    // when the Graphics Debug panel set it.
    m_state.commands.debugView = m_state.renderDebug.gbufferView;
    m_state.commands.gbufferAvailable = !m_state.renderDebug.forwardOnly;
    m_state.commands.toneMapping = m_state.renderDebug.toneMapper;
    m_state.commands.khronosReference = m_state.renderDebug.khronosReference;
    m_state.commands.antiAliasing = m_state.renderDebug.taa ? AntiAliasingMode::Taa : AntiAliasingMode::None;
    // The pipeline is what the settings make of it: path tracing (offline or real time), the ray
    // traced effects over rasterization (hybrid), or rasterization alone.
    m_state.commands.rayTracing = m_state.renderDebug.hardwareRayTracing;
    const PathTracingSettings& pathTracing = m_state.renderDebug.pathTracing;
    m_state.commands.pipelineMode = pathTracing.enabled && pathTracing.offline.enabled ? RenderPipelineMode::PathTracingOffline
                                    : pathTracing.enabled                              ? RenderPipelineMode::PathTracing
                                    : m_state.renderDebug.hardwareRayTracing           ? RenderPipelineMode::Hybrid
                                                                                       : RenderPipelineMode::Rasterization;
    m_state.commands.videoRecording = m_state.videoRecording.active;
    m_state.commands.quadRecording = m_state.quadRecordingStatus.active;
    // Play is driving a car: whatever the commands asked last frame, this is what happened.
    m_state.commands.playState = !m_state.vehicleStatus.active ? PlayState::Stopped
                               : m_state.vehicleStatus.paused ? PlayState::Paused
                                                        : PlayState::Playing;
}

void EditorUiController::ApplyCommandStateToEditor(const EditorCommandState& before, IEditorWorld& scene)
{
    if (m_state.commands.transformTool != before.transformTool)
    {
        GizmoSettings& gizmo = scene.GetGizmoSettings();
        switch (m_state.commands.transformTool)
        {
        case TransformTool::Move:
            gizmo.operation = kCombinedGizmoOperation;
            break;
        case TransformTool::Scale:
            gizmo.operation = ImGuizmo::SCALE;
            break;
        case TransformTool::Rotate:
            gizmo.operation = ImGuizmo::ROTATE;
            break;
        }
    }
    if (m_state.commands.debugView != before.debugView)
    {
        m_state.renderDebug.gbufferView = m_state.commands.debugView;
    }
    if (m_state.commands.toneMapping != before.toneMapping)
    {
        m_state.renderDebug.toneMapper = m_state.commands.toneMapping;
    }
    if (m_state.commands.antiAliasing != before.antiAliasing)
    {
        m_state.renderDebug.taa = m_state.commands.antiAliasing == AntiAliasingMode::Taa;
    }
    if (m_state.commands.rayTracing != before.rayTracing)
    {
        m_state.renderDebug.hardwareRayTracing = m_state.commands.rayTracing;
    }
    if (m_state.commands.pipelineMode != before.pipelineMode)
    {
        const RenderPipelineMode mode = m_state.commands.pipelineMode;
        m_state.renderDebug.pathTracing.enabled = mode == RenderPipelineMode::PathTracing || mode == RenderPipelineMode::PathTracingOffline;
        m_state.renderDebug.pathTracing.offline.enabled = mode == RenderPipelineMode::PathTracingOffline;
        m_state.renderDebug.hardwareRayTracing = mode != RenderPipelineMode::Rasterization;
    }
    if (m_state.commands.playState != before.playState)
    {
        if (before.playState == PlayState::Stopped)
        {
            // Play drives the selected model; the Vehicle panel shows how, or why it could not.
            m_commandActions.startVehicleDrive = true;
            m_windows.Get<VehiclePanel>().Open();
        }
        else if (m_state.commands.playState == PlayState::Stopped)
        {
            m_commandActions.stopVehicleDrive = true;
        }
        else
        {
            m_commandActions.pauseVehicleDrive = m_state.commands.playState == PlayState::Paused;
        }
    }
}

void EditorUiController::AskAboutUnsavedChangesBeforeQuit()
{
    m_windows.Get<UnsavedChangesModal>().Ask(UnsavedChangesModal::Then::Quit);
}

void EditorUiController::HandleFileCommands(EditorContext& context)
{
    IEditorWorld& scene = context.scene;
    EditorUiFrameResult& result = context.result;
    // Each prompt has its own ID, so the typed-path fallback modals do not share one popup.
    // Save goes to the current path if already set; otherwise it asks, like Save As.
    const bool saveToCurrentPath = m_saveSceneRequested && !scene.GetSceneFilePath().empty();
    if (saveToCurrentPath)
    {
        result.actions.selectedSceneSavePath = scene.GetSceneFilePath();
    }
    ImGui::PushID("file.save_scene");
    if (const std::optional<std::string> savePath =
            PickFilePath(FileDialogType::SaveScene, (m_saveSceneRequested && !saveToCurrentPath) || m_saveSceneAsRequested);
        savePath.has_value())
    {
        result.actions.selectedSceneSavePath = *savePath;
    }
    ImGui::PopID();

    ImGui::PushID("file.open_scene");
    if (const std::optional<std::string> loadPath = PickFilePath(FileDialogType::OpenScene, m_openSceneRequested);
        loadPath.has_value())
    {
        result.actions.selectedSceneLoadPath = *loadPath;
    }
    ImGui::PopID();

    // The model goes into the folder the asset browser shows, which opens to show it and to ask
    // when the model's folder is taken.
    ImGui::PushID("file.import_model");
    if (const std::optional<std::string> sourcePath = PickFilePath(FileDialogType::OpenModel, m_importModelRequested);
        sourcePath.has_value())
    {
        AssetBrowserPanel& assets = m_windows.Get<AssetBrowserPanel>();
        assets.Open();
        assets.RequestModelImport(context, *sourcePath);
    }
    ImGui::PopID();

    m_saveSceneRequested = false;
    m_saveSceneAsRequested = false;
    m_openSceneRequested = false;
    m_importModelRequested = false;
}

void EditorUiController::OpenDocumentation()
{
    const std::filesystem::path readme = EnginePaths::ProjectRoot() / "README.md";
    std::error_code error;
    if (!std::filesystem::exists(readme, error))
    {
        LOG_WARN("No documentation at {}", readme.string());
        return;
    }
    OpenInFileBrowser(readme);
}
}
