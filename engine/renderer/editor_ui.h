#pragma once

#include "camera.h"
#include "engine_settings.h"
#include <asset_manager.h>
#include <model_loader.h>
#include <optional>

#include <scene_components.h>
#include <rhi/backend.h>
#include <entt/entt.hpp>

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

    struct ImportedModelRequest
    {
        std::string sourcePath;
        std::string destinationDirectory;
    };

    struct LightCreate
    {
        std::string name;
        LightType type = LightType::Point;
    };

    std::optional<ImportedModelRequest> importedModelRequest;
    std::optional<std::string> selectedModelPath;
    std::optional<std::string> selectedBaseColorTexturePath;
    std::optional<std::string> selectedSceneLoadPath;
    std::optional<std::string> selectedSceneSavePath;
    std::vector<std::string> deleteAssetPaths;
    std::optional<AssetPasteRequest> pastedAsset;
    std::optional<ImportedMaterialUpdate> updatedImportedMaterial;
    std::optional<ImportedModelMaterialsUpdate> updatedImportedModelMaterials;
    std::optional<ViewportModelPlacement> hoveredViewportModel;
    std::optional<ViewportModelPlacement> droppedViewportModel;
    std::optional<LightCreate> createLightEntity;
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
    void ApplyUiScale();
    void CaptureDefaultThemeColors();
    void SyncBaseStyleColorsFromCurrentStyle();
    void ResetThemeColorsToDefault();
    bool DrawThemeEditorWindow();
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
    std::string m_modelProcessorModelPath;
    std::string m_modelProcessorDisplayName;
    std::string m_modelProcessorStatusMessage;
    std::string m_materialPreviewAssetPath;
    std::string m_materialPreviewDisplayName;
    std::string m_materialPreviewModelPath;
    std::string m_materialPreviewStatusMessage;
    LoadedModelData m_modelProcessorLoadedModel;
    std::vector<ModelImportedMaterialInfo> m_modelProcessorMaterials;
    int m_modelProcessorSelectedMaterialIndex = 0;
    int m_modelProcessorSelectedUvSubmeshIndex = 0;
    int m_materialPreviewMaterialIndex = 0;
    uint32_t m_materialGraphSelectedNodeId = 0;
    uint32_t m_materialGraphSelectedLinkId = 0;
    uint32_t m_materialGraphResizeNodeId = 0;
    float m_modelPreviewYaw = 0.55f;
    float m_modelPreviewPitch = 0.35f;
    float m_modelPreviewDistance = 3.0f;
    MaterialGraphNodePosition m_materialGraphContextSpawnPosition{};
    MaterialGraphNodePosition m_materialGraphViewOrigin{};
    MaterialGraphNodePosition m_materialGraphResizeStartPosition{};
    float m_materialGraphZoom = 1.0f;
    ImVec2 m_materialGraphResizeStartMouse{ 0.0f, 0.0f };
    ImVec2 m_materialGraphResizeStartSize{ 0.0f, 0.0f };
    std::optional<MaterialShaderNode> m_materialGraphClipboardNode;
    bool m_modelPreviewAutoFramePending = false;
    bool m_materialGraphLinkDragActive = false;
    bool m_materialGraphNodeResizeActive = false;
    bool m_materialGraphPanningActive = false;
    ModelImportedMaterialInfo m_materialPreviewMaterial;
    std::string m_materialGraphLinkDragFromSlot;
    bool m_openMaterialGraphAddNodePopup = false;
    bool m_showModelProcessorWindow = false;
    bool m_showMaterialPreviewWindow = false;
    bool m_modelProcessorDirty = false;
    uint32_t m_materialGraphLinkDragFromNodeId = 0;
    uint8_t m_materialGraphResizeEdges = 0;
    std::optional<AssetManager> m_assetManager;
    bool m_showCameraWindow = true;
    bool m_showAssetManagerWindow = false;
    bool m_showInputMonitorWindow = false;
    bool m_showSceneWindow = true;
    bool m_showThemeWindow = true;
    bool m_showViewportWindow = true;
    bool m_inputMonitorAutoScroll = true;
};
