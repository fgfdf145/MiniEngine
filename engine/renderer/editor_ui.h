#pragma once

#include "camera.h"
#include "engine_settings.h"
#include "model_loader.h"

#include <scene_components.h>
#include <rhi/backend.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

class IEditorWorld;

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

    struct AssetPasteRequest
    {
        std::string sourcePath;
        std::string destinationDirectory;
    };

    std::optional<std::string> importedModelSourcePath;
    std::optional<std::string> selectedModelPath;
    std::optional<std::string> selectedBaseColorTexturePath;
    std::optional<std::string> selectedSceneLoadPath;
    std::optional<std::string> selectedSceneSavePath;
    std::optional<std::string> deleteAssetPath;
    std::optional<AssetPasteRequest> pastedAsset;
    std::optional<ImportedMaterialUpdate> updatedImportedMaterial;
    std::optional<ImportedModelMaterialsUpdate> updatedImportedModelMaterials;
    std::optional<ViewportModelPlacement> hoveredViewportModel;
    std::optional<ViewportModelPlacement> droppedViewportModel;
    bool createSceneEntity = false;
    bool deleteSelectedSceneEntity = false;
    bool clearSelectedBaseColorTexture = false;
};

struct EditorUiFrameResult
{
    EditorUiActions actions;
    RenderExtent viewportExtent{ 1, 1 };
    SDL_FRect viewportInteractionRect{ 0.0f, 0.0f, 0.0f, 0.0f };
    bool viewportAllowsMouseInteraction = false;
    bool engineSettingsChanged = false;
};

class EditorUiController
{
public:
    void BeginFrame(SDL_Window* window, const EngineSettings& settings);
    void WriteEngineSettings(EngineSettings& settings) const;
    EditorUiFrameResult Draw(
        Camera& camera,
        ViewportMatrices& matrices,
        IEditorWorld& scene,
        const std::string& currentModelPath,
        const std::string& lastLoadError,
        const std::string& lastSceneIoError,
        ImTextureID viewportTextureId,
        RenderExtent viewportExtent,
        RenderBackendType currentBackendType
    );

private:
    void ApplyEngineSettings(const EngineSettings& settings);
    void BeginAssetRename(const std::string& assetPath);
    bool CommitAssetRename(const std::filesystem::path& assetRoot);
    void RequestAssetDelete(const std::string& assetPath);
    void ConfirmRequestedAssetDelete(
        const std::filesystem::path& assetRoot,
        const std::string& normalizedAssetRoot,
        EditorUiFrameResult& result
    );
    void ApplyUiScale();
    void CaptureDefaultThemeColors();
    void SyncBaseStyleColorsFromCurrentStyle();
    void ResetThemeColorsToDefault();
    bool DrawThemeEditorWindow();
    float GetWindowUiScale() const;
    void OpenModelProcessorWindow(const std::string& modelPath);
    void CloseModelProcessorWindow();
    void OpenMaterialPreviewWindow(const std::string& materialPath);
    void CloseMaterialPreviewWindow();

    SDL_Window* m_window = nullptr;
    float m_uiScale = 1.0f;
    float m_effectiveUiScale = 1.0f;
    ImGuiStyle m_baseStyle{};
    std::array<ImVec4, ImGuiCol_COUNT> m_defaultThemeColors{};
    bool m_hasCapturedBaseStyle = false;
    bool m_hasCapturedDefaultThemeColors = false;
    bool m_hasAppliedEngineSettings = false;
    std::string m_copiedAssetPath;
    std::string m_selectedAssetPath;
    std::string m_assetBrowserDirectory;
    std::string m_assetDirectoryTreeExpandedPath;
    std::string m_pendingDuplicateImportSourcePath;
    std::string m_pendingDuplicateImportAssetPath;
    std::string m_modelProcessorModelPath;
    std::string m_modelProcessorDisplayName;
    std::string m_modelProcessorStatusMessage;
    std::string m_materialPreviewAssetPath;
    std::string m_materialPreviewDisplayName;
    std::string m_materialPreviewModelPath;
    std::string m_materialPreviewStatusMessage;
    std::string m_assetRenameTargetPath;
    std::string m_assetRenameError;
    std::string m_pendingDeleteAssetPath;
    LoadedModelData m_modelProcessorLoadedModel;
    std::vector<ModelImportedMaterialInfo> m_modelProcessorMaterials;
    std::vector<std::string> m_modelProcessorMaterialAssetPaths;
    int m_modelProcessorSelectedMaterialIndex = 0;
    int m_modelProcessorSelectedUvSubmeshIndex = 0;
    int m_materialPreviewMaterialIndex = 0;
    float m_modelPreviewYaw = 0.55f;
    float m_modelPreviewPitch = 0.35f;
    float m_modelPreviewDistance = 3.0f;
    bool m_modelPreviewAutoFramePending = false;
    ModelImportedMaterialInfo m_materialPreviewMaterial;
    std::array<char, 256> m_assetRenameBuffer{};
    bool m_openDuplicateImportPopup = false;
    bool m_openAssetRenamePopup = false;
    bool m_openDeleteAssetPopup = false;
    bool m_focusAssetRenameInput = false;
    bool m_showModelProcessorWindow = false;
    bool m_showMaterialPreviewWindow = false;
    bool m_modelProcessorDirty = false;
    bool m_selectedAssetIsDirectory = false;
    bool m_showCameraWindow = true;
    bool m_showAssetManagerWindow = true;
    bool m_showInputMonitorWindow = false;
    bool m_showSceneWindow = true;
    bool m_showThemeWindow = true;
    bool m_showViewportWindow = true;
    bool m_inputMonitorAutoScroll = true;
};
