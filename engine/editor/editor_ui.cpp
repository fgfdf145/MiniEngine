#include "editor_ui.h"
#include "ui/editor_menu_toolbar.h"
#include "ui/editor_suspension_rigs.h"
#include "ui/editor_ui_internal.h"

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/logic/editor_world.h>
#include <engine/platform/ui/ui_scale.h>
#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <ImGuizmo.h>

#include <cmath>
#include <filesystem>
#include <system_error>
#include <utility>

namespace me
{

EditorUiController::EditorUiController()
{
    RegisterCommands();
}

void EditorUiController::RegisterCommands()
{
    // The Window menu lists these, in this order. A new panel needs a line here and nothing else.
    m_panels = {
        {"scene", "Scene", ICON_FA_SITEMAP, &m_showSceneWindow},
        {"viewport", "Viewport", ICON_FA_DISPLAY, &m_showViewportWindow},
        {"camera", "Camera", ICON_FA_VIDEO, &m_showCameraWindow},
        {"graphics_debug", "Graphics Debug", ICON_FA_BUG, &m_showGraphicsDebugWindow},
        {"assets", "Assets", ICON_FA_FOLDER_TREE, &m_showAssetManagerWindow},
        {"input_monitor", "Input Monitor", ICON_FA_KEYBOARD, &m_showInputMonitorWindow},
        {"vehicle", "Vehicle", ICON_FA_CAR, &m_showVehicleWindow},
        {"suspension_rigs", "Suspension Rigs", ICON_FA_CHART_LINE, &m_showSuspensionRigWindow},
        {"theme", "Theme", ICON_FA_PALETTE, &m_showThemeWindow},
    };

    EditorWindowCommands window;
    window.panels = m_panels;
    window.resetLayout = [this]
    {
        m_resetDockLayoutRequested = true;
    };

    EditorSceneCommands scene;
    scene.newScene = [this]
    {
        m_pendingSceneReset = SceneReset::New;
        m_openSceneResetModal = true;
    };
    scene.clearScene = [this]
    {
        m_pendingSceneReset = SceneReset::Clear;
        m_openSceneResetModal = true;
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
        return m_hasSceneSelection && !m_vehicleStatus.active;
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
    scene.stepSimulation = [this]
    {
        m_commandActions.stepVehicleDrive = true;
    };
    scene.openPreferences = [this]
    {
        m_showPreferencesWindow = true;
        FocusWindowWhenDrawn("Preferences");
    };
    // The scene's environment, fog and clouds are edited in the Scene panel.
    scene.openSceneSettings = [this]
    {
        m_showSceneWindow = true;
        FocusWindowWhenDrawn("Scene");
    };
    scene.showDocumentation = [this]
    {
        OpenDocumentation();
    };
    scene.showKeyboardShortcuts = [this]
    {
        m_showKeyboardShortcutsWindow = true;
        FocusWindowWhenDrawn("Keyboard Shortcuts");
    };
    scene.showAbout = [this]
    {
        m_openAboutModal = true;
    };
    RegisterEditorCommands(m_commands, m_commandState, window, scene);
    m_toolbarLayout = BuildEditorToolbarLayout();
}

void EditorUiController::BeginFrame(SDL_Window* window, const EngineSettings& settings)
{
    m_window = window;
    if (!m_hasCapturedBaseStyle)
    {
        m_baseStyle = ImGui::GetStyle();
        m_hasCapturedBaseStyle = true;
    }
    if (!m_hasCapturedDefaultThemeColors)
    {
        CaptureDefaultThemeColors();
        m_hasCapturedDefaultThemeColors = true;
    }
    if (!m_hasAppliedEngineSettings)
    {
        ApplyEngineSettings(settings);
        m_hasAppliedEngineSettings = true;
    }

    ApplyUiScale();
    ImGuizmo::BeginFrame();
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
    const float previousUiScale = m_uiScale;
    // Every Window-menu window's open state, to save the settings when one opens or closes.
    std::vector<bool> previousOpen;
    for (const EditorPanel& panel : m_panels)
    {
        previousOpen.push_back(*panel.visible);
    }

    // Close the processor once its model is gone. Checking the disk every frame is a filesystem
    // call per frame for a file that rarely changes, so it is checked twice a second.
    constexpr double kModelProcessorExistsCheckIntervalSeconds = 0.5;
    const double now = ImGui::GetTime();
    if (m_showModelProcessorWindow &&
        now - m_modelProcessorLastExistsCheckTime >= kModelProcessorExistsCheckIntervalSeconds)
    {
        m_modelProcessorLastExistsCheckTime = now;
        std::error_code processorErrorCode;
        const std::filesystem::path processorModelPath(m_modelProcessorModelPath);
        if (m_modelProcessorModelPath.empty() ||
            !std::filesystem::exists(processorModelPath, processorErrorCode) ||
            processorErrorCode ||
            !IsSupportedModelAssetPath(processorModelPath))
        {
            CloseModelProcessorWindow();
        }
    }

    // Shortcuts first, so what they change shows in this frame's menus and panels. The menu bar
    // and the toolbar come before the dock space, which fills the area they leave.
    m_hasSceneSelection = scene.HasSelection();
    SyncCommandStateFromEditor(scene);
    const EditorCommandState commandStateBefore = m_commandState;
    ProcessCommandShortcuts(m_commands);
    // Escape leaves the fullscreen viewport as F11 does: with no menu there is nothing else to click.
    // An open popup (a typed-path prompt, the command palette) takes Escape to close itself instead.
    if (m_commandState.viewportFullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        m_commandState.viewportFullscreen = false;
    }
    UpdateWindowFullscreen();
    const bool fullscreen = m_commandState.viewportFullscreen;
    if (!fullscreen)
    {
        DrawMainMenu(m_commands);
        DrawToolbar(m_commands, m_toolbarLayout, m_effectiveUiScale);
    }
    // Before the command state is applied, so what a command picked here shows this frame.
    if (std::exchange(m_commandState.commandPaletteRequested, false))
    {
        m_commandPalette.Open();
    }
    m_commandPalette.Draw(m_commands, m_effectiveUiScale);
    ApplyCommandStateToEditor(commandStateBefore, scene);
    if (!fullscreen)
    {
        DrawEditorDockspace(std::exchange(m_resetDockLayoutRequested, false));
    }
    result.actions = std::exchange(m_commandActions, {});
    HandleFileCommands(scene, result);
    DrawSceneResetConfirmModal(result);

    // The fullscreen viewport is all there is: every panel waits for it to end.
    if (!fullscreen && m_showCameraWindow)
    {
        DrawCameraPanel(camera);
    }

    if (!fullscreen && m_showGraphicsDebugWindow)
    {
        DrawGraphicsDebugPanel();
    }

    bool themeChanged = false;
    if (!fullscreen && m_showThemeWindow)
    {
        themeChanged = DrawThemeEditorWindow();
    }

    if (!fullscreen && m_showModelProcessorWindow)
    {
        DrawModelProcessorPanel(scene, result);
    }

    if (!fullscreen && m_showInputMonitorWindow)
    {
        DrawInputMonitorPanel();
    }

    if (!fullscreen && m_showVehicleWindow)
    {
        DrawVehiclePanel(scene, result);
    }

    if (!fullscreen && m_showSuspensionRigWindow)
    {
        if (!m_suspensionRigs)
        {
            m_suspensionRigs = std::make_shared<SuspensionRigWindow>();
        }
        m_suspensionRigs->Draw(scene, &m_showSuspensionRigWindow, m_vehicleRigStatus, result);
    }
    if (m_suspensionRigs)
    {
        // Also when the window is not drawn (fullscreen): the running rig keeps its settings.
        result.vehicleRigExcitation = m_suspensionRigs->Excitation();
    }
    if (!m_showSuspensionRigWindow && m_vehicleRigStatus.active)
    {
        // Closing the window takes the car off the rig.
        result.actions.stopVehicleRig = true;
    }

    if (!fullscreen && m_showSceneWindow)
    {
        DrawScenePanel(scene, lastLoadError, lastSceneIoError, sceneUploadStatus, result);
    }

    if (fullscreen || m_showViewportWindow)
    {
        DrawViewportPanel(camera, matrices, scene, viewportTextureId, currentBackendType, result);
    }

    if (!fullscreen && m_showAssetManagerWindow)
    {
        DrawAssetBrowserPanel(result);
    }

    if (!fullscreen && m_showPreferencesWindow)
    {
        DrawPreferencesWindow();
    }
    DrawHelpWindows(fullscreen);
    // Once every window has been drawn, so one opened this frame exists to be focused.
    if (!fullscreen && !m_focusWindowRequest.empty())
    {
        ImGui::SetWindowFocus(m_focusWindowRequest.c_str());
        m_focusWindowRequest.clear();
    }

    bool windowToggled = false;
    for (size_t index = 0; index < m_panels.size(); ++index)
    {
        windowToggled = windowToggled || previousOpen[index] != *m_panels[index].visible;
    }
    result.engineSettingsChanged = themeChanged || std::abs(previousUiScale - m_uiScale) > 0.0001f || windowToggled;

    result.renderDebug = m_renderDebug;
    result.vehicleTuning = m_vehicleTuning;
    result.vehicleCamera = m_vehicleCamera;
    result.vehicleHaptics = m_vehicleHaptics;
    result.vehicleSteeringAssist = m_vehicleSteeringAssist;
    result.vehicleManualGearbox = m_vehicleManualGearbox;
    return result;
}

void EditorUiController::UpdateWindowFullscreen()
{
    if (m_commandState.viewportFullscreen == m_windowFullscreen)
    {
        return;
    }
    m_windowFullscreen = m_commandState.viewportFullscreen;
    m_fullscreenEnteredTime = ImGui::GetTime();
    // SDL's fullscreen without a display mode is the borderless one at the desktop's resolution.
    if (m_window != nullptr && !SDL_SetWindowFullscreen(m_window, m_windowFullscreen))
    {
        LOG_WARN("Could not switch the window to {}: {}", m_windowFullscreen ? "fullscreen" : "windowed", SDL_GetError());
    }
}

void EditorUiController::SyncCommandStateFromEditor(const IEditorWorld& scene)
{
    const ImGuizmo::OPERATION operation = scene.GetGizmoSettings().operation;
    m_commandState.transformTool = operation == ImGuizmo::SCALE    ? TransformTool::Scale
                                   : operation == ImGuizmo::ROTATE ? TransformTool::Rotate
                                                                   : TransformTool::Move;
    // Every debug view has a View command, so the menu and the toolbar show whichever is set, also
    // when the Graphics Debug panel set it.
    m_commandState.debugView = m_renderDebug.gbufferView;
    m_commandState.gbufferAvailable = !m_renderDebug.forwardOnly;
    m_commandState.toneMapping = m_renderDebug.toneMapper;
    m_commandState.khronosReference = m_renderDebug.khronosReference;
    m_commandState.antiAliasing = m_renderDebug.taa ? AntiAliasingMode::Taa : AntiAliasingMode::None;
    m_commandState.videoRecording = m_videoRecording.active;
    // Play is driving a car: whatever the commands asked last frame, this is what happened.
    m_commandState.playState = !m_vehicleStatus.active ? PlayState::Stopped
                               : m_vehicleStatus.paused ? PlayState::Paused
                                                        : PlayState::Playing;
}

void EditorUiController::ApplyCommandStateToEditor(const EditorCommandState& before, IEditorWorld& scene)
{
    if (m_commandState.transformTool != before.transformTool)
    {
        GizmoSettings& gizmo = scene.GetGizmoSettings();
        switch (m_commandState.transformTool)
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
    if (m_commandState.debugView != before.debugView)
    {
        m_renderDebug.gbufferView = m_commandState.debugView;
    }
    if (m_commandState.toneMapping != before.toneMapping)
    {
        m_renderDebug.toneMapper = m_commandState.toneMapping;
    }
    if (m_commandState.antiAliasing != before.antiAliasing)
    {
        m_renderDebug.taa = m_commandState.antiAliasing == AntiAliasingMode::Taa;
    }
    if (m_commandState.playState != before.playState)
    {
        if (before.playState == PlayState::Stopped)
        {
            // Play drives the selected model; the Vehicle panel shows how, or why it could not.
            m_commandActions.startVehicleDrive = true;
            m_showVehicleWindow = true;
        }
        else if (m_commandState.playState == PlayState::Stopped)
        {
            m_commandActions.stopVehicleDrive = true;
        }
        else
        {
            m_commandActions.pauseVehicleDrive = m_commandState.playState == PlayState::Paused;
        }
    }
}

void EditorUiController::HandleFileCommands(IEditorWorld& scene, EditorUiFrameResult& result)
{
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
        if (!m_assetManager.has_value())
        {
            m_assetManager.emplace(EnginePaths::AssetsRoot());
        }
        m_showAssetManagerWindow = true;
        RequestModelImport(*sourcePath, result);
    }
    ImGui::PopID();

    m_saveSceneRequested = false;
    m_saveSceneAsRequested = false;
    m_openSceneRequested = false;
    m_importModelRequested = false;
}

void EditorUiController::DrawSceneResetConfirmModal(EditorUiFrameResult& result)
{
    constexpr const char* kTitle = "Discard Scene Contents?";
    if (m_openSceneResetModal)
    {
        ImGui::OpenPopup(kTitle);
        m_openSceneResetModal = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    const bool newScene = m_pendingSceneReset == SceneReset::New;
    if (newScene)
    {
        ImGui::TextUnformatted("Start a new scene? Every entity is removed and the scene is no longer tied to its file;");
        ImGui::TextUnformatted("a sun and the default atmosphere are added.");
    }
    else
    {
        ImGui::TextUnformatted("Remove every entity from the scene? The environment and the scene's file are kept.");
    }
    ImGui::TextUnformatted("Changes not saved to the scene file are lost. This cannot be undone.");
    ImGui::Separator();

    const float uiScale = ImGui::GetStyle().FontScaleMain;
    if (ImGui::Button(newScene ? "New Scene" : "Clear Scene", ImVec2(120.0f * uiScale, 0.0f)))
    {
        result.actions.newScene = newScene;
        result.actions.clearScene = !newScene;
        m_pendingSceneReset.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f * uiScale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_pendingSceneReset.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

std::string EditorUiController::PanelSettingsKey(const EditorPanel& panel)
{
    // The asset browser was saved as "asset_manager" before every window was.
    return std::string(panel.id) == "assets" ? "asset_manager" : std::string(panel.id);
}

void EditorUiController::ApplyEngineSettings(const EngineSettings& settings)
{
    m_uiScale = platform::ui::ResolveConfiguredUiScale(settings.editorUi.scale);
    for (const EditorPanel& panel : m_panels)
    {
        const auto open = settings.editorUi.windows.open.find(PanelSettingsKey(panel));
        if (open != settings.editorUi.windows.open.end())
        {
            *panel.visible = open->second;
        }
    }

    if (settings.editorUi.theme.hasCustomColors)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
        {
            if (!settings.editorUi.theme.colorDefined[static_cast<size_t>(colorIndex)])
            {
                continue;
            }
            style.Colors[colorIndex] = settings.editorUi.theme.colors[static_cast<size_t>(colorIndex)];
        }
        SyncBaseStyleColorsFromCurrentStyle();
    }
}

void EditorUiController::WriteEngineSettings(EngineSettings& settings) const
{
    settings.version = 1;
    platform::ui::SetConfiguredUiScaleForCurrentPlatform(settings.editorUi.scale, m_uiScale);
    for (const EditorPanel& panel : m_panels)
    {
        settings.editorUi.windows.open[PanelSettingsKey(panel)] = *panel.visible;
    }
    settings.editorUi.theme.hasCustomColors = true;

    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        settings.editorUi.theme.colors[static_cast<size_t>(colorIndex)] = style.Colors[colorIndex];
        settings.editorUi.theme.colorDefined[static_cast<size_t>(colorIndex)] = true;
    }
}

void EditorUiController::ApplyUiScale()
{
    ImGuiStyle& style = ImGui::GetStyle();
    m_effectiveUiScale = platform::ui::ClampUiScale(platform::ui::ResolveWindowUiScale(m_window) * m_uiScale);

    if (std::abs(style.FontScaleMain - m_effectiveUiScale) <= 0.001f)
    {
        return;
    }

    style = m_baseStyle;
    style.ScaleAllSizes(m_effectiveUiScale);
    style.FontScaleMain = m_effectiveUiScale;
}
}
