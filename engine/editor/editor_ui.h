#pragma once

#include "command_registry.h"
#include "editor_commands.h"
#include "engine_settings.h"
#include "services/vehicle_drive_service.h"
#include "services/vehicle_rig_service.h"
#include "ui/framework/editor_context.h"
#include "ui/framework/editor_style.h"
#include "ui/framework/editor_window_manager.h"

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
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
        // The slots to save; all of them when empty and nothing is restored.
        std::vector<uint32_t> indices;
        // The slots back to the material their import made, whose saved definitions are removed.
        std::vector<uint32_t> restoredIndices;
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
    // glTF animations: what the selected model plays (ModelComponent::animationClip and the rest).
    struct ModelAnimationChoice
    {
        std::string clip;
        bool enabled = true;
        bool playing = true;
        float speed = 1.0f;
        bool springBones = true;
    };
    std::optional<ModelAnimationChoice> selectedModelAnimation;
    // The car the selected model drives (ModelComponent::driverVehicleUuid, empty for none) and its
    // seat offset.
    struct ModelDriverChoice
    {
        std::string vehicleUuid;
        glm::vec3 seatOffset{0.0f};
        DriverGripCalibration grip;
    };
    std::optional<ModelDriverChoice> selectedModelDriver;
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
    // Starts filming the car from four sides into one video under ProjectRoot()/captures, or stops.
    bool toggleQuadRecording = false;
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
    // ABS and traction control switched on or off (abs, traction control) in the panel: the car being
    // driven takes it at once.
    std::optional<std::array<bool, 2>> driverAids;
    // The live seven-post rig (VehicleRigService): put the selected car on it, or take it off.
    bool startVehicleRig = false;
    bool stopVehicleRig = false;
};

struct EditorUiFrameResult
{
    EditorUiActions actions;
    RenderExtent viewportExtent{1, 1};
    // The scene's output pixels per display pixel the viewport shows them on: the render scale, or a
    // fixed resolution's size over the size it is shown at.
    float viewportOutputScale = 1.0f;
    SDL_FRect viewportInteractionRect{0.0f, 0.0f, 0.0f, 0.0f};
    bool viewportAllowsMouseInteraction = false;
    bool engineSettingsChanged = false;
    RenderDebugSettings renderDebug;
    // The Preferences window's master volume and mute.
    EngineAudioSettings audio;
    // The Preferences window's priority and CPUs, set only in the frame they changed.
    std::optional<platform::process::ProcessAllocation> processAllocation;
    // The Vehicle panel's tuning, which the next drive starts with, and its chase camera.
    VehicleSettings vehicleTuning;
    VehicleCameraSettings vehicleCamera;
    VehicleHapticsSettings vehicleHaptics;
    VehicleSteeringAssistSettings vehicleSteeringAssist;
    bool vehicleManualGearbox = false;
    // The Suspension Rigs window's settings for the live rig, taken while it runs. Unset when the
    // window has never been opened: the rig then keeps what it has.
    std::optional<VehicleRigExcitation> vehicleRigExcitation;
    // The Quad Recording window's cameras, and whether it shows their pictures (the backend then
    // renders them even while nothing is recorded).
    QuadRecordingSettings quadRecording;
    bool quadRecordingPreview = false;
};

// The editor shell, as Unreal's level editor or Unity's main window: the main menu, the toolbar, the
// command palette and the dock space, with every window in the EditorWindowManager drawn inside.
// The backend talks only to this class; the windows (ui/panels, ui/windows, ui/modals) talk to
// one another through the manager and to the backend through EditorUiFrameResult.
class EditorUiController
{
  public:
    EditorUiController();
    // The registered commands and windows hold pointers into the controller, so it stays where it
    // was made.
    EditorUiController(const EditorUiController&) = delete;
    EditorUiController& operator=(const EditorUiController&) = delete;

    void BeginFrame(SDL_Window* window, const EngineSettings& settings);
    void WriteEngineSettings(EngineSettings& settings) const;
    // Window DPI scale times the user's UI scale multiplier; the ImGui style is scaled by it.
    float GetEffectiveUiScale() const
    {
        return m_style.EffectiveUiScale();
    }
    // The Graphics Debug settings, for switches set from the command line before the first frame.
    RenderDebugSettings& EditRenderDebug()
    {
        return m_state.renderDebug;
    }
    // The Vehicle panel's camera settings, likewise.
    VehicleCameraSettings& EditVehicleCamera()
    {
        return m_state.vehicle.camera;
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
    void RequestAssetBrowserRefresh();

    // A file or folder dropped onto the editor window from the OS. It is imported (models) or copied
    // into the folder the asset browser shows, and the browser is opened to show it.
    void QueueDroppedFile(std::string path);

    // Whether a car is being driven, for the play controls and the Vehicle panel. Set before Draw.
    void SetVehicleDriveStatus(VehicleDriveStatus status)
    {
        m_state.vehicleStatus = std::move(status);
    }
    // Whether the live seven-post rig runs, and what it recorded, for the Suspension Rigs window.
    void SetVehicleRigStatus(VehicleRigStatus status)
    {
        m_state.vehicleRigStatus = std::move(status);
    }
    // Why the models seated in cars are not sitting in them (VehicleDriverService::Problem), for the
    // Inspector.
    void SetDriverProblems(std::unordered_map<entt::entity, std::string> problems)
    {
        m_state.driverProblems = std::move(problems);
    }
    // The seated drivers' grip frames (VehicleDriverState::grips), for the gizmo on a wrist.
    void SetDriverGrips(std::unordered_map<entt::entity, DriverGripFrames> grips)
    {
        m_state.driverGrips = std::move(grips);
    }
    // The scene's minimap picture as the backend registered it with ImGui; null when there is none.
    void SetMinimapTexture(ImTextureID texture)
    {
        m_state.minimapTexture = texture;
    }
    // The selection outline the renderer draws for this frame (selection_outline_pass.h), the
    // viewport's size, transparent but for the line; drawn over the viewport image. Null when the
    // backend has none, and the selection is then shown by its bounding box.
    void SetSelectionOutlineTexture(ImTextureID texture)
    {
        m_state.selectionOutlineTexture = texture;
    }
    // A line on GPU memory and the world's streaming radius for the Graphics Debug window.
    void SetGpuMemoryStatus(std::string status)
    {
        m_state.gpuMemoryStatus = std::move(status);
    }
    // Whether the render backend can run DLSS, and what the Graphics Debug window says of it
    // (VulkanDlss::Status).
    void SetDlssStatus(bool available, bool rayReconstructionAvailable, std::string status)
    {
        m_state.dlssAvailable = available;
        m_state.dlssRayReconstructionAvailable = rayReconstructionAvailable;
        m_state.dlssStatus = std::move(status);
    }
    // The display and its HDR output, for the display calibration and Graphics Debug's Output section.
    void SetDisplayStatus(EditorDisplayStatus status)
    {
        m_state.display = std::move(status);
    }
    // Whether the render backend can path trace and use hardware ray tracing (the Render > Pipeline
    // modes and the Ray Tracing switch), and what the Graphics Debug window says of path tracing.
    void SetPathTracingStatus(bool available, std::string status)
    {
        m_state.pathTracingAvailable = available;
        m_state.commands.rayTracingSupported = available;
        m_state.pathTracingStatus = std::move(status);
    }
    // DLSS resolves the viewport (see ResolveSceneExtents in the Vulkan renderer): it picks the render
    // size itself, so the viewport asks for every display pixel whatever the render scale says.
    bool DlssResolves() const
    {
        return m_state.DlssResolves();
    }
    // The size the backend renders the scene at whatever the viewport asks, which the viewport then
    // shows at its own aspect (RendererSharedState::fixedViewportExtent).
    void SetForcedViewportExtent(std::optional<RenderExtent> extent)
    {
        m_state.forcedViewportExtent = extent;
    }
    // Whether the viewport is being recorded, for Tools > Record Viewport and the viewport's REC sign.
    void SetVideoRecordingStatus(VideoRecordingIndicator status)
    {
        m_state.videoRecording = std::move(status);
    }
    // The quad recording and what its cameras follow, for the Quad Recording window and Tools >
    // Record Quad Cameras.
    void SetQuadRecordingStatus(VideoRecordingIndicator status, std::string target)
    {
        m_state.quadRecordingStatus = std::move(status);
        m_state.quadRecordingTarget = std::move(target);
    }
    // The audio output the Preferences window names: the device, or why there is none.
    void SetAudioStatus(std::string status)
    {
        m_state.audioStatus = std::move(status);
    }
    // What the process runs at now, for the Preferences window.
    void SetProcessStatus(std::string status)
    {
        m_state.processStatus = std::move(status);
    }

  private:
    // Every window the editor has, in drawing order; the panels' order is the Window menu's.
    void RegisterWindows();
    void RegisterCommands();
    // The command state the scene and the renderer decide (transform tool, debug view,
    // anti-aliasing), read before the commands run and written back after for what they changed.
    void SyncCommandStateFromEditor(const IEditorWorld& scene);
    // Makes the window fullscreen or windowed as the command state asks.
    void UpdateWindowFullscreen();
    void ApplyCommandStateToEditor(const EditorCommandState& before, IEditorWorld& scene);
    // Runs the File commands asked for this frame, asking for a path where they need one.
    void HandleFileCommands(EditorContext& context);
    void OpenDocumentation();

    SDL_Window* m_window = nullptr;
    bool m_hasAppliedEngineSettings = false;
    EditorStyle m_style;
    EditorSharedState m_state;
    EditorWindowManager m_windows;

    // Main menu, toolbar and shortcuts, all built from m_commands.
    CommandRegistry m_commands;
    ToolbarLayout m_toolbarLayout;
    CommandPalette m_commandPalette;
    // Whether the window is fullscreen as the command state last asked.
    bool m_windowFullscreen = false;
    bool m_resetDockLayoutRequested = false;
    // What Window > Auto Layout last fitted the docks for (see DrawEditorDockspace).
    ImGuiID m_autoLayoutKey = 0;
    // Set by the commands, handled once the menus, the toolbar and the shortcuts have run.
    bool m_openSceneRequested = false;
    bool m_saveSceneRequested = false;
    bool m_saveSceneAsRequested = false;
    bool m_importModelRequested = false;
    EditorUiActions m_commandActions; // what the Edit and Scene commands asked for this frame
    bool m_hasSceneSelection = false; // at the start of this frame, for Delete
};
}
