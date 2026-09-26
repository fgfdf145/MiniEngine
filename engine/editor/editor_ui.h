#pragma once

#include "command_registry.h"
#include "editor_commands.h"
#include "engine_settings.h"

#include <engine/renderer/camera.h>
#include <engine/asset/asset_manager.h>
#include <engine/asset/model_import_target.h>
#include <engine/logic/gizmo_settings.h>
#include <engine/asset/model_loader.h>
#include <optional>

#include <engine/scene/scene_components.h>
#include <engine/renderer/rhi/backend.h>
#include <entt/entt.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace me
{

class IEditorWorld;

struct EditorUiActions
{
    struct ViewportModelPlacement
    {
        std::string modelPath;
        glm::vec3 worldPosition{0.0f, 0.0f, 0.0f};
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
        ImportConflictPolicy policy = ImportConflictPolicy::FailIfExists;
    };

    struct LightCreate
    {
        std::string name;
        LightType type = LightType::Point;
    };

    std::optional<ImportedModelRequest> importedModelRequest;
    std::optional<std::string> selectedModelPath;
    std::vector<std::string> batchLoadModelPaths; // each placed as a new scene entity
    std::optional<std::string> selectedBaseColorTexturePath;
    // KHR_materials_variants: the variant picked for the selected model, empty for its default.
    std::optional<std::string> selectedMaterialVariant;
    std::optional<std::string> selectedSceneLoadPath;
    std::optional<std::string> selectedSceneSavePath;
    std::vector<std::string> deleteAssetPaths;
    std::optional<AssetPasteRequest> pastedAsset;
    std::vector<AssetManagerResult::RenamedAsset> renamedAssets; // completed on disk
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
    RenderExtent viewportExtent{1, 1};
    SDL_FRect viewportInteractionRect{0.0f, 0.0f, 0.0f, 0.0f};
    bool viewportAllowsMouseInteraction = false;
    bool engineSettingsChanged = false;
    RenderDebugSettings renderDebug;
};

class EditorUiController
{
  public:
    EditorUiController();
    // The registered commands hold pointers into the controller, so it stays where it was made.
    EditorUiController(const EditorUiController&) = delete;
    EditorUiController& operator=(const EditorUiController&) = delete;

    void BeginFrame(SDL_Window* window, const EngineSettings& settings);
    void WriteEngineSettings(EngineSettings& settings) const;
    // Window DPI scale times the user's UI scale multiplier; the ImGui style is scaled by it.
    float GetEffectiveUiScale() const
    {
        return m_effectiveUiScale;
    }
    // The Graphics Debug settings, for switches set from the command line before the first frame.
    RenderDebugSettings& EditRenderDebug()
    {
        return m_renderDebug;
    }
    EditorUiFrameResult Draw(
        Camera& camera,
        ViewportMatrices& matrices,
        IEditorWorld& scene,
        const std::string& currentModelPath,
        const std::string& lastLoadError,
        const std::string& lastSceneIoError,
        const std::string& sceneUploadStatus,
        ImTextureID viewportTextureId,
        RenderExtent viewportExtent,
        RenderBackendType currentBackendType);

    // Rescans the asset browser on its next draw. Called by the backend when a
    // background operation (e.g. async import) changes files on disk.
    void RequestAssetBrowserRefresh()
    {
        if (m_assetManager.has_value())
        {
            m_assetManager->Refresh();
        }
    }

    // A file or folder dropped onto the editor window from the OS. It is imported (models) or copied
    // into the folder the asset browser shows, and the browser is opened to show it.
    void QueueDroppedFile(std::string path);

  private:
    void RegisterCommands();
    void ApplyEngineSettings(const EngineSettings& settings);
    void ApplyUiScale();
    void CaptureDefaultThemeColors();
    void SyncBaseStyleColorsFromCurrentStyle();
    void ResetThemeColorsToDefault();
    bool DrawThemeEditorWindow();
    void OpenModelProcessorWindow(const std::string& modelPath);
    void CloseModelProcessorWindow();

    // Per-panel draw methods, one translation unit each under ui/.
    void DrawCameraPanel(Camera& camera);
    void DrawGraphicsDebugPanel();
    void DrawInputMonitorPanel();
    void DrawModelProcessorPanel(IEditorWorld& scene, EditorUiFrameResult& result);
    void DrawScenePanel(
        IEditorWorld& scene,
        const std::string& lastLoadError,
        const std::string& lastSceneIoError,
        const std::string& sceneUploadStatus,
        EditorUiFrameResult& result);
    void DrawViewportPanel(
        Camera& camera,
        ViewportMatrices& matrices,
        IEditorWorld& scene,
        ImTextureID viewportTextureId,
        RenderBackendType currentBackendType,
        EditorUiFrameResult& result);
    void DrawAssetBrowserPanel(EditorUiFrameResult& result);
    void DrawImportConflictModal(EditorUiFrameResult& result);
    // Imports a model into the folder being browsed, asking first when its target folder is taken.
    void RequestModelImport(const std::string& sourcePath, EditorUiFrameResult& result);

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
    LoadedModelData m_modelProcessorLoadedModel;
    std::vector<ModelImportedMaterialInfo> m_modelProcessorMaterials;
    int m_modelProcessorSelectedMaterialIndex = 0;
    int m_modelProcessorSelectedUvSubmeshIndex = 0;
    uint32_t m_materialGraphSelectedNodeId = 0;
    uint32_t m_materialGraphSelectedLinkId = 0;
    uint32_t m_materialGraphResizeNodeId = 0;
    float m_modelPreviewYaw = 0.55f;
    float m_modelPreviewPitch = 0.35f;
    float m_modelPreviewDistance = 3.0f;
    GizmoDragSnapState m_gizmoDragSnapState;
    MaterialGraphNodePosition m_materialGraphContextSpawnPosition{};
    MaterialGraphNodePosition m_materialGraphViewOrigin{};
    MaterialGraphNodePosition m_materialGraphResizeStartPosition{};
    float m_materialGraphZoom = 1.0f;
    ImVec2 m_materialGraphResizeStartMouse{0.0f, 0.0f};
    ImVec2 m_materialGraphResizeStartSize{0.0f, 0.0f};
    std::optional<MaterialShaderNode> m_materialGraphClipboardNode;
    bool m_modelPreviewAutoFramePending = false;
    bool m_materialGraphLinkDragActive = false;
    bool m_materialGraphNodeResizeActive = false;
    bool m_materialGraphPanningActive = false;
    std::string m_materialGraphLinkDragFromSlot;
    bool m_openMaterialGraphAddNodePopup = false;
    bool m_showModelProcessorWindow = false;
    bool m_modelProcessorDirty = false;
    double m_modelProcessorLastExistsCheckTime = -1.0e9;
    uint32_t m_materialGraphLinkDragFromNodeId = 0;
    uint8_t m_materialGraphResizeEdges = 0;
    std::optional<AssetManager> m_assetManager;
    // An import whose model folder already holds files, waiting for the user
    // to choose keep-both, overwrite or cancel.
    struct PendingImportConflict
    {
        std::string sourcePath;
        std::string destinationDirectory;
        std::string existingFolderName;
        std::string keepBothFolderName;
    };
    std::optional<PendingImportConflict> m_pendingImportConflict;
    bool m_openImportConflictModal = false;
    std::deque<std::string> m_droppedFiles; // queued by QueueDroppedFile, drained by the asset browser
    bool m_showCameraWindow = true;
    RenderDebugSettings m_renderDebug;
    bool m_showAssetManagerWindow = false;
    bool m_showInputMonitorWindow = false;
    bool m_showSceneWindow = true;
    bool m_showThemeWindow = true;
    bool m_showViewportWindow = true;
    bool m_showGraphicsDebugWindow = false;
    bool m_inputMonitorAutoScroll = true;
    std::vector<std::string> m_inputMonitorMessages;
    uint64_t m_inputMonitorMessagesRevision = 0;

    // Main menu, toolbar and shortcuts, all built from m_commands.
    CommandRegistry m_commands;
    EditorCommandState m_commandState;
    std::vector<EditorPanel> m_panels; // what the Window menu shows and hides
    ToolbarLayout m_toolbarLayout;
    bool m_resetDockLayoutRequested = false;
};
}
