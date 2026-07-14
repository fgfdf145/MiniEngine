#include "editor_ui.h"
#include "ui/editor_ui_internal.h"

#include <material_graph_runtime.h>
#include <model_loader.h>
#include <texture_loader.h>

#include <editor_world.h>
#include <file_dialog/file_dialog.h>
#include <log/log.h>
#include <ui/ui_scale.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <yaml-cpp/yaml.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

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
    ImTextureID viewportTextureId,
    RenderExtent viewportExtent,
    RenderBackendType currentBackendType
)
{
    static_cast<void>(currentModelPath);
    static_cast<void>(lastLoadError);

    EditorUiFrameResult result{};
    result.viewportExtent = viewportExtent;
    const float previousUiScale = m_uiScale;
    const bool previousShowCameraWindow = m_showCameraWindow;
    const bool previousShowAssetManagerWindow = m_showAssetManagerWindow;
    const bool previousShowInputMonitorWindow = m_showInputMonitorWindow;
    const bool previousShowSceneWindow = m_showSceneWindow;
    const bool previousShowThemeWindow = m_showThemeWindow;
    const bool previousShowViewportWindow = m_showViewportWindow;

    if (m_showModelProcessorWindow)
    {
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

    const float toolbarHeight = 44.0f * m_effectiveUiScale;
    DrawDockspaceBelowToolbar(toolbarHeight);

    DrawTopToolbar(
        m_showCameraWindow,
        m_showAssetManagerWindow,
        m_showInputMonitorWindow,
        m_showSceneWindow,
        m_showThemeWindow,
        m_showViewportWindow,
        m_effectiveUiScale
    );

    if (m_showCameraWindow)
    {
        DrawCameraPanel(camera);
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
        DrawScenePanel(scene, lastSceneIoError, result);
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
        previousShowViewportWindow != m_showViewportWindow;

    return result;
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
