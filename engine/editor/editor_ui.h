#pragma once

#include "command_registry.h"
#include "editor_commands.h"
#include "engine_settings.h"
#include "services/vehicle_drive_service.h"
#include "services/vehicle_rig_service.h"

#include <engine/asset/asset_manager.h>
#include <engine/asset/model_import_target.h>
#include <engine/asset/model_loader.h>
#include <engine/core/threading/task_future.h>
#include <engine/logic/gizmo_settings.h>
#include <engine/renderer/camera.h>
#include <engine/renderer/rhi/backend.h>
#include <engine/scene/scene_components.h>

#include <entt/entt.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace me
{

class IEditorWorld;
class SuspensionRigWindow;

// What the viewport shows of a video recording (Tools > Record Viewport).
struct VideoRecordingIndicator
{
    bool active = false;
    // The video's length so far, its files' size and the frames left out while the encoders were
    // behind.
    double seconds = 0.0;
    uint64_t bytes = 0;
    uint64_t droppedFrames = 0;
    // How the last recording ended (where it was saved, or why it stopped), shown for a few seconds
    // from messageTime.
    std::string message;
    bool messageIsError = false;
    std::chrono::steady_clock::time_point messageTime{};
};

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
        // The slots to save; all of them when empty.
        std::vector<uint32_t> indices;
    };

    // Materials edited in the Model Preview window, shown in the scene but not saved: each with
    // its slot index.
    struct ImportedModelMaterialPreview
    {
        std::string modelPath;
        std::vector<std::pair<uint32_t, ModelImportedMaterialInfo>> materials;
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
        // What the Assetto Corsa import dialog chose; unused for other formats.
        Kn5ImportOptions kn5Options;
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
    // KHR_lights_punctual: whether the selected model's own lights shine.
    std::optional<bool> selectedUseModelLights;
    std::optional<std::string> selectedSceneLoadPath;
    // A sound file the Assets window asked to hear: played, or stopped when it is already playing.
    std::optional<std::string> previewAudioPath;
    std::optional<std::string> selectedSceneSavePath;
    std::vector<std::string> deleteAssetPaths;
    std::optional<AssetPasteRequest> pastedAsset;
    std::vector<AssetManagerResult::RenamedAsset> renamedAssets; // completed on disk
    std::optional<ImportedModelMaterialsUpdate> updatedImportedModelMaterials;
    std::optional<ImportedModelMaterialPreview> previewImportedModelMaterial;
    // The model whose previewed material edits are dropped: it is read again from disk.
    std::optional<std::string> revertImportedModelMaterials;
    std::optional<ViewportModelPlacement> hoveredViewportModel;
    std::optional<ViewportModelPlacement> droppedViewportModel;
    std::optional<LightCreate> createLightEntity;
    bool createSceneEntity = false;
    bool deleteSelectedSceneEntity = false;
    // Points the viewport camera at the selected entity.
    bool frameSelectedSceneEntity = false;
    bool captureViewport = false; // written as a PNG under ProjectRoot()/captures
    // Starts recording the viewport to an AVI under ProjectRoot()/captures, or stops.
    bool toggleVideoRecording = false;
    bool newScene = false;        // confirmed by the user
    bool clearScene = false;      // confirmed by the user
    bool clearSelectedBaseColorTexture = false;
    // Play mode (VehicleDriveService): drive the selected model as a car, and stop, pause, step,
    // reset or recover it.
    bool startVehicleDrive = false;
    bool stopVehicleDrive = false;
    std::optional<bool> pauseVehicleDrive;
    bool stepVehicleDrive = false;
    bool resetVehicle = false;
    bool recoverVehicle = false; // back on its wheels where it is
    // The view the car is seen from: chase, cockpit, bonnet or bumper.
    std::optional<VehicleCameraView> vehicleCameraView;
    // The brush tyre's cut changed in the tuning (ribs, segments per rib; 0 for the tyre's own): the car
    // being driven takes it at once.
    std::optional<std::array<int, 2>> brushTyreBristles;
    // The live seven-post rig (VehicleRigService): put the selected car on it, or take it off.
    bool startVehicleRig = false;
    bool stopVehicleRig = false;
};

struct EditorUiFrameResult
{
    EditorUiActions actions;
    RenderExtent viewportExtent{1, 1};
    SDL_FRect viewportInteractionRect{0.0f, 0.0f, 0.0f, 0.0f};
    bool viewportAllowsMouseInteraction = false;
    bool engineSettingsChanged = false;
    RenderDebugSettings renderDebug;
    // The Preferences window's master volume and mute.
    EngineAudioSettings audio;
    // The Vehicle panel's tuning, which the next drive starts with, and its chase camera.
    VehicleSettings vehicleTuning;
    VehicleCameraSettings vehicleCamera;
    VehicleHapticsSettings vehicleHaptics;
    VehicleSteeringAssistSettings vehicleSteeringAssist;
    bool vehicleManualGearbox = false;
    // The Suspension Rigs window's settings for the live rig, taken while it runs. Unset when the
    // window has never been opened: the rig then keeps what it has.
    std::optional<VehicleRigExcitation> vehicleRigExcitation;
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

    // Whether a car is being driven, for the play controls and the Vehicle panel. Set before Draw.
    void SetVehicleDriveStatus(VehicleDriveStatus status)
    {
        m_vehicleStatus = std::move(status);
    }
    // Whether the live seven-post rig runs, and what it recorded, for the Suspension Rigs window.
    void SetVehicleRigStatus(VehicleRigStatus status)
    {
        m_vehicleRigStatus = std::move(status);
    }
    // Whether the viewport is being recorded, for Tools > Record Viewport and the viewport's REC sign.
    // The scene's minimap picture as the backend registered it with ImGui; null when there is none.
    void SetMinimapTexture(ImTextureID texture)
    {
        m_minimapTexture = texture;
    }
    // The selection outline the renderer draws for this frame (selection_outline_pass.h), the
    // viewport's size, transparent but for the line; drawn over the viewport image. Null when the
    // backend has none, and the selection is then shown by its bounding box.
    void SetSelectionOutlineTexture(ImTextureID texture)
    {
        m_selectionOutlineTexture = texture;
    }
    // A line on GPU memory and the world's streaming radius for the Graphics Debug window.
    void SetGpuMemoryStatus(std::string status)
    {
        m_gpuMemoryStatus = std::move(status);
    }
    // Whether the render backend can run DLSS, and what the Graphics Debug window says of it
    // (VulkanDlss::Status).
    void SetDlssStatus(bool available, bool rayReconstructionAvailable, std::string status)
    {
        m_dlssAvailable = available;
        m_dlssRayReconstructionAvailable = rayReconstructionAvailable;
        m_dlssStatus = std::move(status);
    }
    // DLSS resolves the viewport (see ResolveSceneExtents in the Vulkan renderer): it picks the render
    // size itself, so the viewport asks for every display pixel whatever the render scale says.
    bool DlssResolves() const
    {
        return m_dlssAvailable && m_renderDebug.dlssMode != DlssMode::Off && !m_renderDebug.forwardOnly;
    }
    void SetVideoRecordingStatus(VideoRecordingIndicator status)
    {
        m_videoRecording = std::move(status);
    }
    // The audio output the Preferences window names: the device, or why there is none.
    void SetAudioStatus(std::string status)
    {
        m_audioStatus = std::move(status);
    }

  private:
    void RegisterCommands();
    // The command state the scene and the renderer decide (transform tool, debug view,
    // anti-aliasing), read before the commands run and written back after for what they changed.
    void SyncCommandStateFromEditor(const IEditorWorld& scene);
    // Makes the window fullscreen or windowed as the command state asks.
    void UpdateWindowFullscreen();
    void ApplyCommandStateToEditor(const EditorCommandState& before, IEditorWorld& scene);
    // Runs the File commands asked for this frame, asking for a path where they need one.
    void HandleFileCommands(IEditorWorld& scene, EditorUiFrameResult& result);
    // Asks before New Scene or Clear Scene throws the scene's contents away.
    void DrawSceneResetConfirmModal(EditorUiFrameResult& result);
    // The Help menu's windows, the Preferences window and the window focus they ask for.
    void DrawHelpWindows(bool fullscreen);
    void DrawPreferencesWindow();
    void OpenDocumentation();
    void FocusWindowWhenDrawn(std::string windowName);
    void ApplyEngineSettings(const EngineSettings& settings);
    // The key a Window-menu window's open state is saved under.
    static std::string PanelSettingsKey(const EditorPanel& panel);
    void ApplyUiScale();
    void CaptureDefaultThemeColors();
    void SyncBaseStyleColorsFromCurrentStyle();
    void ResetThemeColorsToDefault();
    bool DrawThemeEditorWindow();
    // Opens the window on a model; with preselectPaint the slot most like car paint is selected.
    void OpenModelProcessorWindow(const std::string& modelPath, bool preselectPaint = false);
    void CloseModelProcessorWindow();
    // The previewed edits back to what is on disk, when there are any.
    void RevertModelProcessorPreview(EditorUiFrameResult& result);

    // Per-panel draw methods, one translation unit each under ui/.
    void DrawCameraPanel(Camera& camera);
    void DrawGraphicsDebugPanel();
    void DrawInputMonitorPanel();
    void DrawVehiclePanel(const IEditorWorld& scene, EditorUiFrameResult& result);
    void DrawModelProcessorPanel(IEditorWorld& scene, EditorUiFrameResult& result);
    // The model processor's graph section for one material: toolbar, selection and canvas.
    // Each returns whether the graph changed.
    bool DrawMaterialGraphEditor(ModelImportedMaterialInfo& material, size_t materialIndex);
    bool DrawMaterialGraphCanvas(ModelImportedMaterialInfo& material, size_t materialIndex);
    bool DrawMaterialGraphAddNodePopup(
        ModelImportedMaterialInfo& material,
        bool canPasteClipboardNode,
        std::optional<MaterialGraphNodePosition>& pendingPasteNodePosition);
    void UpdateMaterialGraphView(const ImVec2& canvasOrigin, bool canvasBackgroundHovered);
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
    // Asks for an Assetto Corsa model's livery and conversion options before importing it.
    void DrawKn5ImportModal(EditorUiFrameResult& result);
    // Imports a model into the folder being browsed, asking first when its target folder is taken.
    // A .kn5 first asks for its livery and options unless `kn5Options` already holds them.
    void RequestModelImport(
        const std::string& sourcePath,
        EditorUiFrameResult& result,
        std::optional<Kn5ImportOptions> kn5Options = std::nullopt);

    SDL_Window* m_window = nullptr;
    float m_uiScale = 1.0f;
    float m_effectiveUiScale = 1.0f;
    ImGuiStyle m_baseStyle{};
    std::array<ImVec4, ImGuiCol_COUNT> m_defaultThemeColors{};
    // The palette as ConfigureImGuiStyle set it; the settings file keeps only the colours that differ.
    std::array<ImVec4, ImGuiCol_COUNT> m_builtInThemeColors{};
    bool m_hasCapturedBaseStyle = false;
    bool m_hasCapturedDefaultThemeColors = false;
    bool m_hasAppliedEngineSettings = false;
    GizmoDragSnapState m_gizmoDragSnapState;

    // The model processor: one imported model's materials, edited as graphs and previewed.
    bool m_showModelProcessorWindow = false;
    std::string m_modelProcessorModelPath;
    std::string m_modelProcessorDisplayName;
    std::string m_modelProcessorStatusMessage;
    LoadedModelData m_modelProcessorLoadedModel;
    std::vector<ModelImportedMaterialInfo> m_modelProcessorMaterials;
    int m_modelProcessorSelectedMaterialIndex = 0;
    int m_modelProcessorSelectedUvSubmeshIndex = 0;
    bool m_modelProcessorDirty = false;
    // The slots changed since the last save; previewed in the scene, written by Save.
    std::set<uint32_t> m_modelProcessorEditedSlots;
    // Edits show in the scene as they are made.
    bool m_modelProcessorLivePreview = true;
    // The scene shows edits that are not saved, to be reverted when they are dropped.
    bool m_modelProcessorScenePreviewed = false;
    bool m_focusModelProcessorWindow = false;
    // Quick Edit's base colour brightness, kept between frames so dragging it below 1 does not
    // fold it back into the colour.
    float m_quickEditBrightness = 1.0f;
    double m_modelProcessorLastExistsCheckTime = -1.0e9;
    // Its preview camera, orbiting the model.
    struct ModelPreviewCamera
    {
        float yaw = 0.55f;
        float pitch = 0.35f;
        float distance = 3.0f;
        bool autoFramePending = false;
    };
    ModelPreviewCamera m_modelPreview;
    // Its material graph canvas: selection, drags and view. Reset whenever a model is opened or
    // closed, by assigning a default one.
    struct MaterialGraphCanvas
    {
        uint32_t selectedNodeId = 0;
        uint32_t selectedLinkId = 0;
        MaterialGraphNodePosition viewOrigin{};
        float zoom = 1.0f;
        bool panningActive = false;
        // Where a node added from the context menu goes.
        MaterialGraphNodePosition contextSpawnPosition{};
        bool openAddNodePopup = false;
        bool linkDragActive = false;
        uint32_t linkDragFromNodeId = 0;
        std::string linkDragFromSlot;
        bool nodeResizeActive = false;
        uint32_t resizeNodeId = 0;
        uint8_t resizeEdges = 0;
        MaterialGraphNodePosition resizeStartPosition{};
        ImVec2 resizeStartMouse{0.0f, 0.0f};
        ImVec2 resizeStartSize{0.0f, 0.0f};

        void CancelLinkDrag()
        {
            linkDragActive = false;
            linkDragFromNodeId = 0;
            linkDragFromSlot.clear();
        }
        void CancelResize()
        {
            nodeResizeActive = false;
            resizeNodeId = 0;
            resizeEdges = 0;
        }
    };
    MaterialGraphCanvas m_materialGraph;
    // A copied node. Kept across models, so it can be pasted into another one.
    std::optional<MaterialShaderNode> m_materialGraphClipboardNode;

    std::optional<AssetManager> m_assetManager;
    // An import whose model folder already holds files, waiting for the user
    // to choose keep-both, overwrite or cancel.
    struct PendingImportConflict
    {
        std::string sourcePath;
        std::string destinationDirectory;
        std::string existingFolderName;
        std::string keepBothFolderName;
        Kn5ImportOptions kn5Options;
    };
    std::optional<PendingImportConflict> m_pendingImportConflict;
    bool m_openImportConflictModal = false;
    // A .kn5 or track layout import waiting for its livery, layout and options. The model is surveyed on a background
    // thread: a track's kn5 runs to hundreds of megabytes.
    struct PendingKn5Import
    {
        std::string sourcePath;
        std::string destinationDirectory;
        TaskFuture<Kn5ModelSummary> survey;
        std::optional<Kn5ModelSummary> summary;
        std::string error;
        size_t selectedSkin = 0;
        // 0: the picked file; n: summary->layouts[n - 1].
        size_t selectedLayout = 0;
        Kn5ImportOptions options;
    };
    std::optional<PendingKn5Import> m_pendingKn5Import;
    bool m_openKn5ImportModal = false;
    // Surveys of dialogs cancelled before they finished, kept until done so cancelling never
    // waits on one.
    std::vector<TaskFuture<Kn5ModelSummary>> m_abandonedKn5Surveys;
    std::deque<std::string> m_droppedFiles; // queued by QueueDroppedFile, drained by the asset browser
    bool m_showCameraWindow = true;
    RenderDebugSettings m_renderDebug;
    EngineAudioSettings m_audio;
    std::string m_audioStatus;
    bool m_showAssetManagerWindow = false;
    bool m_showInputMonitorWindow = false;
    bool m_showSceneWindow = true;
    bool m_showThemeWindow = true;
    bool m_showViewportWindow = true;
    bool m_showGraphicsDebugWindow = false;
    bool m_showVehicleWindow = false;
    bool m_showSuspensionRigWindow = false;
    // Created when first shown; shared_ptr so this header needs no complete type.
    std::shared_ptr<SuspensionRigWindow> m_suspensionRigs;
    VehicleRigStatus m_vehicleRigStatus;
    VehicleDriveStatus m_vehicleStatus;
    VideoRecordingIndicator m_videoRecording;
    ImTextureID m_minimapTexture = ImTextureID{};
    ImTextureID m_selectionOutlineTexture = ImTextureID{};
    bool m_dlssAvailable = false;
    bool m_dlssRayReconstructionAvailable = false;
    std::string m_dlssStatus;
    std::string m_gpuMemoryStatus;
    VehicleSettings m_vehicleTuning = VehicleDriveService::DefaultTuning();
    VehicleCameraSettings m_vehicleCamera;
    VehicleHapticsSettings m_vehicleHaptics;
    VehicleSteeringAssistSettings m_vehicleSteeringAssist;
    bool m_vehicleManualGearbox = false;
    VehiclePhysicsOverlaySettings m_vehicleOverlay;
    bool m_inputMonitorAutoScroll = true;
    std::vector<std::string> m_inputMonitorMessages;
    uint64_t m_inputMonitorMessagesRevision = 0;

    // Main menu, toolbar and shortcuts, all built from m_commands.
    CommandRegistry m_commands;
    EditorCommandState m_commandState;
    // Whether the window is fullscreen as the command state last asked, and when that began.
    bool m_windowFullscreen = false;
    double m_fullscreenEnteredTime = 0.0;
    std::vector<EditorPanel> m_panels; // what the Window menu shows and hides
    ToolbarLayout m_toolbarLayout;
    bool m_resetDockLayoutRequested = false;
    // Set by the commands, handled once the menus, the toolbar and the shortcuts have run.
    bool m_openSceneRequested = false;
    bool m_saveSceneRequested = false;
    bool m_saveSceneAsRequested = false;
    bool m_importModelRequested = false;
    enum class SceneReset
    {
        New,
        Clear
    };
    std::optional<SceneReset> m_pendingSceneReset; // waiting for the confirmation modal
    bool m_openSceneResetModal = false;
    CommandPalette m_commandPalette;
    bool m_showKeyboardShortcutsWindow = false;
    bool m_showPreferencesWindow = false;
    bool m_openAboutModal = false;
    // A window a command opened, brought to the front once it has been drawn.
    std::string m_focusWindowRequest;
    EditorUiActions m_commandActions; // what the Edit and Scene commands asked for this frame
    bool m_hasSceneSelection = false; // at the start of this frame, for Delete
};
}
