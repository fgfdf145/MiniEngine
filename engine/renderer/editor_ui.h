#pragma once

#include "camera.h"

#include <rhi/backend.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <optional>
#include <string>

class EditorScene;

struct EditorUiActions
{
    std::optional<std::string> importedModelSourcePath;
    std::optional<std::string> selectedModelPath;
    std::optional<std::string> selectedBaseColorTexturePath;
    std::optional<std::string> selectedSceneLoadPath;
    std::optional<std::string> selectedSceneSavePath;
    std::optional<std::string> deleteAssetPath;
    bool clearSelectedBaseColorTexture = false;
};

struct EditorUiFrameResult
{
    EditorUiActions actions;
    RenderExtent viewportExtent{ 1, 1 };
};

class EditorUiController
{
public:
    void BeginFrame(SDL_Window* window);
    EditorUiFrameResult Draw(
        Camera& camera,
        ViewportMatrices& matrices,
        EditorScene& scene,
        const std::string& currentModelPath,
        const std::string& lastLoadError,
        const std::string& lastSceneIoError,
        ImTextureID viewportTextureId,
        RenderExtent viewportExtent,
        RenderBackendType currentBackendType
    );

private:
    void ApplyUiScale();
    float GetWindowUiScale() const;

    SDL_Window* m_window = nullptr;
    float m_uiScale = 1.0f;
    float m_effectiveUiScale = 1.0f;
    ImGuiStyle m_baseStyle{};
    bool m_hasCapturedBaseStyle = false;
    std::string m_selectedAssetPath;
    bool m_selectedAssetIsDirectory = false;
    bool m_showCameraWindow = true;
    bool m_showAssetManagerWindow = true;
    bool m_showSceneWindow = true;
    bool m_showViewportWindow = true;
};
