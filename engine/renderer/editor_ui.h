#pragma once

#include "camera.h"

#include <scene_components.h>
#include <rhi/backend.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

class EditorScene;

struct EditorUiActions
{
    struct ViewportModelPlacement
    {
        std::string modelPath;
        glm::vec3 worldPosition{ 0.0f, 0.0f, 0.0f };
    };

    struct ImportedMaterialUpdate
    {
        std::string modelPath;
        uint32_t materialIndex = 0;
        ModelImportedMaterialInfo material;
    };

    struct ImportedModelMaterialsUpdate
    {
        std::string modelPath;
        std::vector<ModelImportedMaterialInfo> materials;
    };

    std::optional<std::string> importedModelSourcePath;
    std::optional<std::string> selectedModelPath;
    std::optional<std::string> selectedBaseColorTexturePath;
    std::optional<std::string> selectedSceneLoadPath;
    std::optional<std::string> selectedSceneSavePath;
    std::optional<std::string> deleteAssetPath;
    std::optional<ImportedMaterialUpdate> updatedImportedMaterial;
    std::optional<ImportedModelMaterialsUpdate> updatedImportedModelMaterials;
    std::optional<ViewportModelPlacement> hoveredViewportModel;
    std::optional<ViewportModelPlacement> droppedViewportModel;
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
    void OpenModelProcessorWindow(const std::string& modelPath);
    void CloseModelProcessorWindow();

    SDL_Window* m_window = nullptr;
    float m_uiScale = 1.0f;
    float m_effectiveUiScale = 1.0f;
    ImGuiStyle m_baseStyle{};
    bool m_hasCapturedBaseStyle = false;
    std::string m_selectedAssetPath;
    std::string m_assetBrowserDirectory;
    std::string m_pendingDuplicateImportSourcePath;
    std::string m_pendingDuplicateImportAssetPath;
    std::string m_modelProcessorModelPath;
    std::string m_modelProcessorDisplayName;
    std::string m_modelProcessorStatusMessage;
    std::string m_materialGraphDraftModelPath;
    std::string m_materialGraphNodeLayoutKey;
    std::vector<ModelImportedMaterialInfo> m_modelProcessorMaterials;
    std::vector<std::string> m_modelProcessorMaterialAssetPaths;
    int m_modelProcessorSelectedMaterialIndex = 0;
    int m_materialGraphSelectedIndex = 0;
    int m_materialGraphDraftIndex = -1;
    int m_materialGraphDraggingNodeIndex = -1;
    ModelImportedMaterialInfo m_materialGraphDraft;
    std::array<ImVec2, 4> m_materialGraphNodeLayout{};
    bool m_openDuplicateImportPopup = false;
    bool m_showModelProcessorWindow = false;
    bool m_modelProcessorDirty = false;
    bool m_materialGraphDraftDirty = false;
    bool m_materialGraphNodeLayoutInitialized = false;
    bool m_selectedAssetIsDirectory = false;
    bool m_showCameraWindow = true;
    bool m_showAssetManagerWindow = true;
    bool m_showSceneWindow = true;
    bool m_showViewportWindow = true;
};
