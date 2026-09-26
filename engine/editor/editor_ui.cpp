#include "editor_ui.h"
#include "ui/editor_menu_toolbar.h"
#include "ui/editor_ui_internal.h"

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
        {"theme", "Theme", ICON_FA_PALETTE, &m_showThemeWindow},
    };

    EditorWindowCommands window;
    window.panels = m_panels;
    window.resetLayout = [this]
    {
        m_resetDockLayoutRequested = true;
    };

    EditorSceneCommands scene;
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
    const bool previousShowCameraWindow = m_showCameraWindow;
    const bool previousShowAssetManagerWindow = m_showAssetManagerWindow;
    const bool previousShowInputMonitorWindow = m_showInputMonitorWindow;
    const bool previousShowSceneWindow = m_showSceneWindow;
    const bool previousShowThemeWindow = m_showThemeWindow;
    const bool previousShowViewportWindow = m_showViewportWindow;
    const bool previousShowGraphicsDebugWindow = m_showGraphicsDebugWindow;

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
    DrawMainMenu(m_commands);
    DrawToolbar(m_commands, m_toolbarLayout, m_effectiveUiScale);
    ApplyCommandStateToEditor(commandStateBefore, scene);
    DrawEditorDockspace(std::exchange(m_resetDockLayoutRequested, false));
    result.actions = std::exchange(m_commandActions, {});
    HandleFileCommands(scene, result);
    // TODO: draw the command palette while m_commandState.commandPaletteRequested is set.
    m_commandState.commandPaletteRequested = false;

    if (m_showCameraWindow)
    {
        DrawCameraPanel(camera);
    }

    if (m_showGraphicsDebugWindow)
    {
        DrawGraphicsDebugPanel();
    }

    bool themeChanged = false;
    if (m_showThemeWindow)
    {
        themeChanged = DrawThemeEditorWindow();
    }

    if (m_showModelProcessorWindow)
    {
        DrawModelProcessorPanel(scene, result);
    }

    if (m_showInputMonitorWindow)
    {
        DrawInputMonitorPanel();
    }

    if (m_showSceneWindow)
    {
        DrawScenePanel(scene, lastLoadError, lastSceneIoError, sceneUploadStatus, result);
    }

    if (m_showViewportWindow)
    {
        DrawViewportPanel(camera, matrices, scene, viewportTextureId, currentBackendType, result);
    }

    if (m_showAssetManagerWindow)
    {
        DrawAssetBrowserPanel(result);
    }

    result.engineSettingsChanged =
        themeChanged ||
        std::abs(previousUiScale - m_uiScale) > 0.0001f ||
        previousShowCameraWindow != m_showCameraWindow ||
        previousShowAssetManagerWindow != m_showAssetManagerWindow ||
        previousShowInputMonitorWindow != m_showInputMonitorWindow ||
        previousShowSceneWindow != m_showSceneWindow ||
        previousShowThemeWindow != m_showThemeWindow ||
        previousShowViewportWindow != m_showViewportWindow ||
        previousShowGraphicsDebugWindow != m_showGraphicsDebugWindow;

    result.renderDebug = m_renderDebug;
    return result;
}

void EditorUiController::SyncCommandStateFromEditor(const IEditorWorld& scene)
{
    m_commandState.transformTool =
        scene.GetGizmoSettings().operation == ImGuizmo::SCALE ? TransformTool::Scale : TransformTool::Move;
    // The other G-buffer views have no View command; the menu keeps its last choice for them.
    switch (m_renderDebug.gbufferView)
    {
    case GBufferDebugView::Off:
        m_commandState.debugView = ViewportDebugView::Lit;
        break;
    case GBufferDebugView::Albedo:
        m_commandState.debugView = ViewportDebugView::Albedo;
        break;
    case GBufferDebugView::Normal:
        m_commandState.debugView = ViewportDebugView::Normal;
        break;
    default:
        break;
    }
    m_commandState.antiAliasing = m_renderDebug.taa ? AntiAliasingMode::Taa : AntiAliasingMode::None;
}

void EditorUiController::ApplyCommandStateToEditor(const EditorCommandState& before, IEditorWorld& scene)
{
    // A choice with nothing behind it yet changes nothing, and the next frame's sync shows the
    // editor's own state again.
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
            break;
        }
    }
    if (m_commandState.debugView != before.debugView)
    {
        switch (m_commandState.debugView)
        {
        case ViewportDebugView::Lit:
            m_renderDebug.gbufferView = GBufferDebugView::Off;
            break;
        case ViewportDebugView::Albedo:
            m_renderDebug.gbufferView = GBufferDebugView::Albedo;
            break;
        case ViewportDebugView::Normal:
            m_renderDebug.gbufferView = GBufferDebugView::Normal;
            break;
        default:
            break;
        }
    }
    if (m_commandState.antiAliasing != before.antiAliasing)
    {
        m_renderDebug.taa = m_commandState.antiAliasing == AntiAliasingMode::Taa;
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

void EditorUiController::ApplyEngineSettings(const EngineSettings& settings)
{
    m_uiScale = platform::ui::ResolveConfiguredUiScale(settings.editorUi.scale);
    m_showCameraWindow = settings.editorUi.windows.camera;
    m_showAssetManagerWindow = settings.editorUi.windows.assetManager;
    m_showInputMonitorWindow = settings.editorUi.windows.inputMonitor;
    m_showSceneWindow = settings.editorUi.windows.scene;
    m_showThemeWindow = settings.editorUi.windows.theme;
    m_showViewportWindow = settings.editorUi.windows.viewport;
    m_showGraphicsDebugWindow = settings.editorUi.windows.graphicsDebug;

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
    settings.editorUi.windows.camera = m_showCameraWindow;
    settings.editorUi.windows.assetManager = m_showAssetManagerWindow;
    settings.editorUi.windows.inputMonitor = m_showInputMonitorWindow;
    settings.editorUi.windows.scene = m_showSceneWindow;
    settings.editorUi.windows.theme = m_showThemeWindow;
    settings.editorUi.windows.viewport = m_showViewportWindow;
    settings.editorUi.windows.graphicsDebug = m_showGraphicsDebugWindow;
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
