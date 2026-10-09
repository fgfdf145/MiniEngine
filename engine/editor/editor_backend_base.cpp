#include "editor_backend_base.h"

#include "services/capture_state.h"
#include "services/entity_edit_service.h"
#include "services/model_import_service.h"
#include "services/photo_mode.h"
#include "services/quad_recording.h"
#include "services/scene_io_service.h"
#include "services/scene_renderables.h"
#include "services/vehicle_drive_service.h"
#include "services/vehicle_rig_service.h"
#include "services/world_streaming_service.h"

#include <engine/asset/asset_registry.h>
#include <engine/asset/gta5_importer.h>
#include <engine/asset/kn5_importer.h>
#include <engine/asset/model_cache.h>
#include <engine/asset/model_loader.h>
#include <engine/asset/png_file.h>
#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/core/threading/render_thread.h>
#include <engine/core/threading/task_system.h>
#include <engine/logic/world_bounds.h>
#include <engine/platform/process/process_allocation.h>
#include <engine/platform/window/window.h>
#include <engine/scene/world_units.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace me
{

namespace
{
std::filesystem::path BuildCapturePath(const char* prefix, const char* extension);
std::filesystem::path BuildCapturePath(const char* prefix, const char* extension, const std::filesystem::path& folder);

// Runs one action the UI asked for. A failure goes to `error`, where the UI shows it, and to the
// log as "Failed to <what>: <reason>"; the frame's other actions still run.
template <typename Action>
void RunUiAction(std::string& error, const std::string& what, Action&& action)
{
    try
    {
        action();
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        LOG_ERROR("Failed to {}: {}", what, exception.what());
    }
}
// Runs the scene's clock by the frame's time; setting the environment turns the sun to match.
void AdvanceTimeOfDay(IEditorWorld* world, float deltaSeconds)
{
    if (world == nullptr)
    {
        return;
    }
    const TimeOfDaySettings& time = world->GetEnvironment().timeOfDay;
    if (!time.enabled || !(time.timeScale > 0.0f) || !(deltaSeconds > 0.0f))
    {
        return;
    }
    SceneEnvironment environment = world->GetEnvironment();
    // A long hitch (a load, a breakpoint) must not jump the sun across the sky.
    const float hours = environment.timeOfDay.hours + std::min(deltaSeconds, 0.25f) * time.timeScale / 3600.0f;
    environment.timeOfDay.hours = std::fmod(hours, 24.0f);
    world->SetEnvironment(environment);
}
}

EditorRenderBackendBase::EditorRenderBackendBase(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    RenderBackendType backendType,
    std::optional<std::string> startupModelPath)
    : m_window(window),
      m_sharedState(std::move(sharedState)),
      m_backendType(backendType)
{
    EnsureInitialized(std::move(startupModelPath));
}

RenderBackendType EditorRenderBackendBase::GetBackendType() const
{
    return m_backendType;
}

void EditorRenderBackendBase::HandleEvent(const SDL_Event& event)
{
    State().input.HandleEvent(event);
    HandleBackendEvent(event);

    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr)
    {
        State().editorUi.QueueDroppedFile(event.drop.data);
    }

    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE))
    {
        SDL_SetWindowRelativeMouseMode(m_window.GetSDLWindow(), State().input.WantsRelativeMouseMode());
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == SDL_BUTTON_RIGHT &&
        State().input.ShouldRestoreMouseLookAnchor())
    {
        int anchorX = 0;
        int anchorY = 0;
        State().input.ConsumeMouseLookAnchor(anchorX, anchorY);
        SDL_WarpMouseInWindow(m_window.GetSDLWindow(), static_cast<float>(anchorX), static_cast<float>(anchorY));
    }

    // A right click anywhere in the viewport deselects; holding it to look around does not.
    if (State().input.ConsumeViewportRightClick() && State().editorWorld)
    {
        State().editorWorld->ClearSelection();
    }
}

bool EditorRenderBackendBase::TickSharedFrame()
{
    // MINIENGINE_FIXED_FRAME_SECONDS=<s>: every frame steps that long instead of the time that passed,
    // and no time at all while the scene is still loading, so two scripted runs (A/B captures) see
    // the same clouds, animation and physics however long their loading took.
    static const float fixedFrameSeconds = []
    {
        const char* value = std::getenv("MINIENGINE_FIXED_FRAME_SECONDS");
        return value != nullptr ? std::max(std::strtof(value, nullptr), 0.0f) : 0.0f;
    }();
    const auto currentFrameTime = std::chrono::steady_clock::now();
    const float deltaTime = fixedFrameSeconds > 0.0f ? (State().IsSceneLoading() ? 0.0f : fixedFrameSeconds)
                                                     : std::chrono::duration<float>(currentFrameTime - State().lastFrameTime).count();
    State().lastFrameTime = currentFrameTime;
    State().frameDeltaSeconds = deltaTime;

    State().input.Update();
    // While a car is driven the keyboard and gamepad are its controls: the camera only looks around
    // with the mouse, and not even that while it chases the car.
    const bool keyboardCaptured = WantsKeyboardCapture();
    const bool driving = VehicleDriveService::Tick(State(), deltaTime, keyboardCaptured);
    VehicleRigService::Tick(State(), deltaTime);
    VehicleDriverService::Tick(State(), deltaTime);
    State().modelAnimation.Tick(State(), deltaTime);
    AdvanceTimeOfDay(State().editorWorld.get(), deltaTime);
    if (!driving || !State().vehicleDrive.camera.follow)
    {
        UpdateCameraFromInput(
            State().camera, State().input, deltaTime, keyboardCaptured || driving,
            driving ? std::nullopt : SelectionOrbitPivot());
    }
    State().input.EndFrame();

    if (AudioEngine* const audio = State().audio.get())
    {
        // Heard from the camera, the car's chase camera included.
        const Camera& camera = State().camera;
        audio->SetListener(camera.position, camera.GetForward(), camera.worldUp);
        audio->Update();
    }

    return HasDrawableArea();
}

void EditorRenderBackendBase::PreviewAudio(const std::string& path)
{
    AudioEngine* const audio = State().audio.get();
    if (audio == nullptr)
    {
        LOG_WARN("Cannot play '{}': there is no audio output ({})", path, State().audioStatus);
        return;
    }
    if (!audio->PreviewPath().empty() && audio->PreviewPath() == std::filesystem::path(path))
    {
        audio->StopPreview();
        return;
    }
    std::string error;
    if (!audio->StartPreview(path, error))
    {
        LOG_ERROR("{}", error);
    }
}

bool EditorRenderBackendBase::ProcessPendingOperations()
{
    bool renderablesDirty = State().renderablesDirty;

    if (State().pendingScenePath.has_value())
    {
        const std::string path = *State().pendingScenePath;
        State().pendingScenePath.reset();

        try
        {
            SceneIoService::StartAsyncSceneLoad(State(), path);
        }
        catch (const std::exception& error)
        {
            State().lastSceneIoError = error.what();
            LOG_ERROR("Failed to start loading scene '{}': {}", path, error.what());
        }

        State().lastFrameTime = std::chrono::steady_clock::now();
    }

    // Start at most one queued model load per frame, and only while the async
    // loader is idle, so batch requests drain one after another.
    if (!State().pendingModelLoads.empty() &&
        !State().asyncLoad.IsLoading() &&
        !State().asyncSceneLoad.IsLoading())
    {
        const PendingModelLoad request = State().pendingModelLoads.front();
        State().pendingModelLoads.pop_front();

        try
        {
            std::string path = request.path;

            // Unpacking embedded textures happens at import, so a model that
            // was never imported has none. Import it first; the invariant is
            // that anything reaching the loader lives under the assets root.
            // Assetto Corsa and GTA V files are never loaded as they are: their import converts them.
            if (!AssetRegistry::IsUnderAssetsRoot(path) || Kn5Importer::IsKn5Path(path) ||
                Kn5Importer::IsLayoutPath(path) || Gta5Importer::IsGta5Path(path))
            {
                LOG_INFO("Model '{}' is not an imported glTF; importing it first", path);
                path = ModelImportService::ImportModelIntoAssetDirectory(
                    path, (EnginePaths::AssetsRoot() / "models").string());
            }

            LOG_INFO("Loading model: {}", path);
            if (!request.placeAsNewEntity && EditorWorld().HasSelection())
            {
                EntityEditService::LoadSelectedModel(State(), path);
            }
            else
            {
                EntityEditService::PlaceModelIntoScene(State(), path, glm::vec3(0.0f));
            }
            renderablesDirty = true;
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to load model '{}': {}", request.path, error.what());
        }

        State().lastFrameTime = std::chrono::steady_clock::now();
    }

    renderablesDirty |= EntityEditService::PumpAsyncModelLoad(State());
    renderablesDirty |= SceneIoService::PumpAsyncSceneLoad(State());
    renderablesDirty |= WorldStreamingService::Tick(State());
    ModelImportService::PumpAsyncImport(State());

    State().renderablesDirty = false;
    return renderablesDirty;
}

void EditorRenderBackendBase::ApplyUiActions(const EditorUiFrameResult& uiFrame)
{
    State().requestedViewportExtent = State().fixedViewportExtent.value_or(uiFrame.viewportExtent);
    State().viewportOutputScale = uiFrame.viewportOutputScale;
    if (State().fixedViewportExtent.has_value())
    {
        // The viewport panel built its matrices for its own size; the scene renders at the fixed one.
        UpdateViewportMatrices(*State().fixedViewportExtent);
    }
    State().renderDebug = uiFrame.renderDebug;
    State().quadRecording = uiFrame.quadRecording;
    State().quadRecordingPreview = uiFrame.quadRecordingPreview;
    State().photoMode = uiFrame.photoMode;
    if (AudioEngine* const audio = State().audio.get())
    {
        audio->SetMasterVolume(uiFrame.audio.EffectiveVolume());
    }
    if (uiFrame.processAllocation.has_value())
    {
        const platform::process::ProcessorTopology topology = platform::process::QueryProcessorTopology();
        const platform::process::ProcessAllocationResult applied =
            platform::process::ApplyProcessAllocation(*uiFrame.processAllocation, topology);
        // The affinity moves every thread; the task workers (physics steps, parallel loops) must also
        // shrink or grow to the new CPUs, or they crowd fewer cores or leave new ones idle.
        TaskSystem::SetActiveWorkerThreads(TaskSystem::WorkersForCpus(static_cast<uint32_t>(applied.cpus.size())));
        State().processStatus = platform::process::DescribeProcessAllocation(*uiFrame.processAllocation, applied, topology);
        LOG_INFO("Process: {}", State().processStatus);
    }
    State().input.SetViewportInteractionRegion(
        uiFrame.viewportInteractionRect,
        uiFrame.viewportAllowsMouseInteraction);

    const EditorUiActions& actions = uiFrame.actions;
    std::string& modelError = State().lastModelLoadError;
    std::string& sceneError = State().lastSceneIoError;

    if (actions.previewAudioPath.has_value())
    {
        PreviewAudio(*actions.previewAudioPath);
    }

    State().vehicleDrive.camera = uiFrame.vehicleCamera;
    State().vehicleDrive.haptics = uiFrame.vehicleHaptics;
    State().vehicleDrive.steeringAssist = uiFrame.vehicleSteeringAssist;
    State().vehicleDrive.manualGearbox = uiFrame.vehicleManualGearbox;
    State().vehicleDrive.physicsStepSeconds =
        std::clamp(uiFrame.vehiclePhysicsStepSeconds, PhysicsWorld::kMinStepSeconds, PhysicsWorld::kMaxStepSeconds);
    if (actions.stopVehicleDrive)
    {
        VehicleDriveService::Stop(State());
    }
    if (actions.stopVehicleRig)
    {
        VehicleRigService::Stop(State());
    }
    if (uiFrame.vehicleRigExcitation.has_value())
    {
        VehicleRigService::SetExcitation(State(), *uiFrame.vehicleRigExcitation);
    }
    if (actions.startVehicleRig)
    {
        RunUiAction(State().vehicleRig.lastError, "start the seven-post rig", [&]
                    {
                        // One or the other: the rig and driving both move the car.
                        VehicleDriveService::Stop(State());
                        const entt::entity selected = EditorWorld().HasSelection() ? EditorWorld().GetSelectedEntity() : entt::null;
                        VehicleRigService::Start(State(), selected, uiFrame.vehicleRigExcitation.value_or(VehicleRigExcitation{}));
                    });
    }
    if (actions.startVehicleDrive)
    {
        VehicleRigService::Stop(State());
        RunUiAction(State().vehicleDrive.lastError, "start driving", [&]
                    {
                        const entt::entity selected = EditorWorld().HasSelection() ? EditorWorld().GetSelectedEntity() : entt::null;
                        VehicleDriveService::Start(State(), selected, uiFrame.vehicleTuning);
                    });
    }
    if (actions.followDrivePath.has_value() || actions.replayDriveLog.has_value())
    {
        RunUiAction(State().vehicleDrive.lastError, "drive by itself", [&]
                    {
                        if (!State().vehicleDrive.session)
                        {
                            VehicleRigService::Stop(State());
                            const entt::entity selected = EditorWorld().HasSelection() ? EditorWorld().GetSelectedEntity() : entt::null;
                            VehicleDriveService::Start(State(), selected, uiFrame.vehicleTuning);
                        }
                        if (actions.followDrivePath.has_value())
                        {
                            VehicleDriveService::StartPathFollow(State(), actions.followDrivePath->path, actions.followDrivePath->track);
                        }
                        else
                        {
                            VehicleDriveService::StartReplay(State(), std::filesystem::path(*actions.replayDriveLog));
                        }
                    });
    }
    if (actions.stopDriveAutomation)
    {
        VehicleDriveService::StopAutomation(State());
    }
    if (actions.driveLog.has_value())
    {
        RunUiAction(State().vehicleDrive.lastError, "write the drive down", [&]
                    {
                        if (*actions.driveLog)
                        {
                            VehicleDriveService::StartDriveLog(State(), BuildCapturePath("drive", ".csv"), true);
                        }
                        else
                        {
                            VehicleDriveService::StopDriveLog(State());
                        }
                    });
    }
    if (actions.pauseVehicleDrive.has_value())
    {
        VehicleDriveService::SetPaused(State(), *actions.pauseVehicleDrive);
    }
    if (actions.brushTyreBristles.has_value())
    {
        VehicleDriveService::SetBrushTyreBristles(State(), (*actions.brushTyreBristles)[0], (*actions.brushTyreBristles)[1]);
    }
    if (actions.driverAids.has_value())
    {
        VehicleDriveService::SetDriverAids(State(), (*actions.driverAids)[0], (*actions.driverAids)[1]);
    }
    if (actions.stepVehicleDrive)
    {
        VehicleDriveService::Step(State());
    }
    if (actions.resetVehicle)
    {
        VehicleDriveService::Reset(State());
    }
    if (actions.recoverVehicle)
    {
        VehicleDriveService::Recover(State());
    }
    if (actions.vehicleCameraView.has_value())
    {
        VehicleDriveService::SetCameraView(State(), *actions.vehicleCameraView);
    }

    RunUiAction(modelError, "update viewport model preview", [&]
                {
                    if (actions.hoveredViewportModel.has_value())
                    {
                        EntityEditService::UpdateViewportModelPreview(
                            State(), actions.hoveredViewportModel->modelPath, actions.hoveredViewportModel->worldPosition);
                    }
                    else
                    {
                        EntityEditService::ClearViewportModelPreview(State());
                    }
                });
    if (const auto& request = actions.importedModelRequest)
    {
        RunUiAction(modelError, fmt::format("start import of '{}' into '{}'", request->sourcePath, request->destinationDirectory), [&]
                    {
                        ModelImportService::StartAsyncImport(
                            State(), request->sourcePath, request->destinationDirectory, request->policy, request->kn5Options);
                    });
    }
    if (actions.selectedModelPath.has_value())
    {
        State().pendingModelLoads.push_back({*actions.selectedModelPath, false});
    }
    for (const std::string& path : actions.batchLoadModelPaths)
    {
        State().pendingModelLoads.push_back({path, true});
    }
    if (actions.createSceneEntity)
    {
        RunUiAction(modelError, "create scene entity", [&]
                    {
                        EntityEditService::CreateSceneEntity(State());
                    });
    }
    if (const auto& light = actions.createLightEntity)
    {
        RunUiAction(modelError, "create light entity", [&]
                    {
                        EntityEditService::CreateSceneLightEntity(State(), light->name, light->type);
                    });
    }
    if (actions.frameSelectedSceneEntity)
    {
        EntityEditService::FrameSelectedEntity(State());
    }
    if (actions.deleteSelectedSceneEntity)
    {
        RunUiAction(modelError, "delete selected entity", [&]
                    {
                        if (EditorWorld().HasSelection() && EditorWorld().HasLightComponent(EditorWorld().GetSelectedEntity()))
                        {
                            EntityEditService::DeleteSelectedLightEntity(State());
                        }
                        else
                        {
                            EntityEditService::DeleteSelectedSceneEntity(State());
                        }
                    });
    }
    if (const auto& dropped = actions.droppedViewportModel)
    {
        RunUiAction(modelError, fmt::format("place dropped model '{}' into scene", dropped->modelPath), [&]
                    {
                        EntityEditService::CommitViewportModelPreview(State(), dropped->modelPath, dropped->worldPosition);
                    });
    }
    if (const auto& update = actions.updatedImportedModelMaterials)
    {
        RunUiAction(modelError, fmt::format("update imported model materials '{}'", update->modelPath), [&]
                    {
                        ModelImportService::UpdateImportedModelMaterialDefinitions(
                            State(), update->modelPath, update->materials, update->indices, update->restoredIndices);
                    });
    }
    if (const auto& preview = actions.previewImportedModelMaterial)
    {
        RunUiAction(modelError, fmt::format("preview edited materials of '{}'", preview->modelPath), [&]
                    {
                        ModelImportService::PreviewImportedModelMaterials(State(), preview->modelPath, preview->materials);
                    });
    }
    if (const auto& revert = actions.revertImportedModelMaterials)
    {
        RunUiAction(modelError, fmt::format("revert material edits of '{}'", *revert), [&]
                    {
                        ModelImportService::RevertImportedModelMaterials(State(), *revert);
                    });
    }
    if (const auto& texture = actions.selectedBaseColorTexturePath)
    {
        RunUiAction(modelError, fmt::format("apply base color texture '{}' to selected model", *texture), [&]
                    {
                        EntityEditService::ApplySelectedModelBaseColorTexture(State(), *texture);
                    });
    }
    if (const auto& variant = actions.selectedMaterialVariant)
    {
        RunUiAction(modelError, fmt::format("apply material variant '{}' to selected model", *variant), [&]
                    {
                        EntityEditService::ApplySelectedModelMaterialVariant(State(), *variant);
                    });
    }
    if (const auto& animation = actions.selectedModelAnimation)
    {
        RunUiAction(modelError, "change the selected model's animation", [&]
                    {
                        EntityEditService::ApplySelectedModelAnimation(
                            State(), animation->clip, animation->enabled, animation->playing, animation->speed, animation->springBones);
                    });
    }
    if (const auto& driver = actions.selectedModelDriver)
    {
        RunUiAction(modelError, "seat the selected model in a car", [&]
                    {
                        EntityEditService::ApplySelectedModelDriver(State(), driver->vehicleUuid, driver->seatOffset, driver->grip);
                    });
    }
    if (const auto& useModelLights = actions.selectedUseModelLights)
    {
        RunUiAction(modelError, "toggle the selected model's lights", [&]
                    {
                        EntityEditService::ApplySelectedModelUseModelLights(State(), *useModelLights);
                    });
    }
    if (actions.clearSelectedBaseColorTexture)
    {
        RunUiAction(modelError, "clear selected model texture override", [&]
                    {
                        EntityEditService::ClearSelectedModelBaseColorTexture(State());
                    });
    }
    if (actions.selectedSceneLoadPath.has_value())
    {
        State().pendingScenePath = *actions.selectedSceneLoadPath;
    }
    if (actions.newScene || actions.clearScene)
    {
        RunUiAction(sceneError, "reset the scene", [&]
                    {
                        if (actions.newScene)
                        {
                            SceneIoService::NewScene(State());
                        }
                        else
                        {
                            SceneIoService::ClearScene(State());
                        }
                    });
    }
    if (actions.captureViewport)
    {
        CaptureViewportWithState();
    }
    if (actions.toggleVideoRecording)
    {
        ToggleVideoRecordingFromEditor();
    }
    UpdateVideoRecording();
    if (actions.toggleQuadRecording)
    {
        ToggleQuadRecordingFromEditor();
    }
    UpdateQuadRecording();
    if (actions.takePhoto)
    {
        TakePhotoFromEditor();
    }
    if (const auto& savePath = actions.selectedSceneSavePath)
    {
        RunUiAction(sceneError, fmt::format("save scene '{}'", *savePath), [&]
                    {
                        VehicleDriveService::RunWithVehicleAtStart(State(), [&]
                                                                   {
                                                                       VehicleRigService::RunWithRigAtStart(State(), [&]
                                                                                                            {
                                                                                                                SceneIoService::SaveScene(State(), *savePath);
                                                                                                            });
                                                                   });
                    });
    }
    for (const AssetManagerResult::RenamedAsset& renamed : actions.renamedAssets)
    {
        ModelImportService::OnAssetRenamed(State(), renamed.oldPath, renamed.newPath);
    }
    for (const std::string& deletePath : actions.deleteAssetPaths)
    {
        RunUiAction(modelError, fmt::format("delete asset '{}'", deletePath), [&]
                    {
                        ModelImportService::DeleteAssetPath(deletePath);
                    });
    }
    if (const auto& paste = actions.pastedAsset)
    {
        RunUiAction(modelError, fmt::format("paste asset '{}' into '{}'", paste->sourcePath, paste->destinationDirectory), [&]
                    {
                        ModelImportService::PasteAsset(paste->sourcePath, paste->destinationDirectory);
                    });
    }
    // Last: the actions above may have stopped the drive or changed the selection the cameras follow.
    UpdateCaptureViews();
}

namespace
{
// ProjectRoot()/captures/<prefix>_<local date>_<time><extension>.
std::filesystem::path BuildCapturePath(const char* prefix, const char* extension)
{
    return BuildCapturePath(prefix, extension, EnginePaths::ProjectRoot() / "captures");
}

// The same in another folder.
std::filesystem::path BuildCapturePath(const char* prefix, const char* extension, const std::filesystem::path& folder)
{
    SDL_DateTime now{};
    SDL_Time ticks = 0;
    if (!SDL_GetCurrentTime(&ticks) || !SDL_TimeToDateTime(ticks, &now, true))
    {
        now = SDL_DateTime{};
    }
    char name[96];
    std::snprintf(
        name, sizeof(name), "%s_%04d%02d%02d_%02d%02d%02d%s",
        prefix, now.year, now.month, now.day, now.hour, now.minute, now.second, extension);
    return folder / name;
}

std::string FormatMegabytes(uint64_t bytes)
{
    return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}
}

// The viewport as it is on screen, before this frame records, to
// captures/viewport_<local date>_<time>.png, with what it takes to render it again beside it:
// viewport_<...>.scene.yaml and viewport_<...>.state.yaml, replayed with miniengine_app --state.
void EditorRenderBackendBase::CaptureViewportWithState()
{
    const std::filesystem::path path = BuildCapturePath("viewport", ".png");
    try
    {
        std::filesystem::create_directories(path.parent_path());
        CaptureViewport(path);
        // Beside the image, what it takes to render it again: viewport_<...>.scene.yaml and
        // viewport_<...>.state.yaml, replayed with miniengine_app --state.
        std::filesystem::path scenePath = path;
        scenePath.replace_extension(".scene.yaml");
        std::filesystem::path statePath = path;
        statePath.replace_extension(".state.yaml");
        SceneIoService::ExportSceneSnapshot(State(), scenePath.string());
        CaptureState captureState;
        captureState.scenePath = scenePath.filename();
        captureState.originalScenePath = EditorWorld().GetSceneFilePath();
        captureState.viewportExtent = State().fixedViewportExtent.value_or(State().requestedViewportExtent);
        captureState.camera = State().camera;
        captureState.renderDebug = State().renderDebug;
        CaptureStateService::Write(statePath, captureState);
        LOG_INFO("Wrote the capture's scene and state to '{}'", statePath.string());
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("Failed to capture the viewport to '{}': {}", path.string(), error.what());
    }
}

bool EditorRenderBackendBase::TakePhoto(const PhotoRequest& request, std::string& error)
{
    if (m_photo.has_value())
    {
        error = "A photo is already being made";
        return false;
    }
    PhotoModeSettings asked;
    asked.width = request.width;
    asked.height = request.height;
    asked.warmupFrames = request.warmupFrames;
    asked.samplesPerPixel = request.samplesPerPixel;
    asked.targetSamples = request.targetSamples;
    const PhotoModeSettings clamped = ClampPhotoModeSettings(asked);
    PhotoInProgress photo;
    photo.request = request;
    photo.request.width = clamped.width;
    photo.request.height = clamped.height;
    photo.request.warmupFrames = clamped.warmupFrames;
    photo.request.samplesPerPixel = clamped.samplesPerPixel;
    photo.request.targetSamples = clamped.targetSamples;
    // The moment the shutter is pressed: every tile is of this view, wherever the camera goes next.
    photo.camera = State().camera;
    photo.viewportAspect = m_viewportAspect;
    photo.tiling = PlanPhotoTiles(
        photo.request.width,
        photo.request.height,
        request.maxViewPixels != 0
            ? request.maxViewPixels
            : PhotoMaxViewPixels(
                  State().gpuMemory,
                  PhotoExtraBytesPerPixel(photo.request.offlinePathTracing, photo.request.dlssMode, photo.request.dlssRayReconstruction)));
    if (photo.tiling.Tiled())
    {
        photo.canvas.assign(static_cast<size_t>(photo.request.width) * photo.request.height * 4, 0);
    }
    PhotoStatus& status = State().photoStatus;
    status = PhotoStatus{};
    status.rendering = true;
    status.framesTotal = PhotoTileFrames(photo.request) * static_cast<uint32_t>(photo.tiling.tiles.size());
    status.targetSamples = photo.request.offlinePathTracing ? photo.request.targetSamples : 0u;
    status.tile = 1;
    status.tileCount = static_cast<uint32_t>(photo.tiling.tiles.size());
    status.width = photo.request.width;
    status.height = photo.request.height;
    const RenderExtent viewExtent = photo.tiling.ViewExtent();
    status.viewWidth = viewExtent.width;
    status.viewHeight = viewExtent.height;
    LOG_INFO(
        "Photo: {}x{} in {} tile(s) ({}x{} views), {}, {}, to '{}'",
        photo.request.width,
        photo.request.height,
        photo.tiling.tiles.size(),
        viewExtent.width,
        viewExtent.height,
        photo.request.offlinePathTracing
            ? fmt::format("path traced to {} spp ({} a frame) then {} frames", photo.request.targetSamples, photo.request.samplesPerPixel,
                          photo.request.warmupFrames)
            : fmt::format("{} frames each", photo.request.warmupFrames),
        photo.request.dlssMode == DlssMode::Off ? std::string("TAA")
                                                : fmt::format("DLSS mode {}{}", static_cast<int>(photo.request.dlssMode),
                                                              photo.request.dlssRayReconstruction ? " with ray reconstruction" : ""),
        request.path.string());
    m_photoViewReport.reset();
    m_photo = std::move(photo);
    return true;
}

void EditorRenderBackendBase::TakePhotoFromEditor()
{
    const PhotoModeSettings settings = ClampPhotoModeSettings(State().photoMode);
    PhotoRequest request = PhotoRequestFromSettings(settings);
    request.path = BuildCapturePath("photo", ".png", PhotoFolder(settings));
    std::string error;
    if (!TakePhoto(request, error))
    {
        PhotoStatus& status = State().photoStatus;
        status.message = error;
        status.messageIsError = true;
        status.messageTime = std::chrono::steady_clock::now();
        LOG_WARN("Photo: {}", error);
    }
}

void EditorRenderBackendBase::AdvancePhoto()
{
    if (!m_photo.has_value())
    {
        return;
    }
    PhotoInProgress& photo = *m_photo;
    PhotoStatus& status = State().photoStatus;
    const auto finish = [&](std::string error)
    {
        const PhotoRequest request = photo.request;
        const size_t tiles = photo.tiling.tiles.size();
        const float exposure = photo.exposureEv100;
        m_photo.reset();
        status.rendering = false;
        status.saving = false;
        status.messageTime = std::chrono::steady_clock::now();
        status.messageIsError = !error.empty();
        if (error.empty())
        {
            status.message = fmt::format("Saved {} x {} to {}", request.width, request.height, request.path.string());
            status.lastPhoto = request.path;
            LOG_INFO(
                "Photo: saved {}x{} ({} tile(s)) to '{}' at EV100 {:.2f}", request.width, request.height, tiles, request.path.string(), exposure);
        }
        else
        {
            status.message = fmt::format("The photo was not saved: {}", error);
            LOG_ERROR("Photo: could not write '{}': {}", request.path.string(), error);
        }
    };

    // Written: report it.
    if (photo.writing.has_value())
    {
        if (photo.writing->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            finish(photo.writing->get());
        }
        return;
    }
    if (!PhotoTileDone(photo))
    {
        return;
    }

    // The tile's view has rendered its frames: its picture into the canvas.
    try
    {
        PhotoViewPicture picture = ReadPhotoView();
        const RenderExtent expected = photo.tiling.ViewExtent();
        if (picture.width != expected.width || picture.height != expected.height)
        {
            throw std::runtime_error(fmt::format(
                "the photo's view was {}x{}, not the {}x{} it was asked for", picture.width, picture.height, expected.width, expected.height));
        }
        photo.exposureEv100 = picture.exposureEv100;
        if (photo.tiling.Tiled())
        {
            CopyTileIntoCanvas(photo.tiling, photo.tiling.tiles[photo.tile], picture.rgba, photo.canvas);
        }
        else
        {
            photo.canvas = std::move(picture.rgba);
        }
    }
    catch (const std::exception& error)
    {
        finish(error.what());
        return;
    }
    ++photo.tile;
    photo.tileFrames = 0;
    photo.settledFrames = 0;
    if (photo.tile < photo.tiling.tiles.size())
    {
        return;
    }

    // Every tile is in: the PNG is written on a worker, which an 8K one keeps busy for seconds.
    status.rendering = false;
    status.saving = true;
    photo.writing = std::async(
        std::launch::async,
        [path = photo.request.path, width = photo.request.width, height = photo.request.height, canvas = std::move(photo.canvas)]() -> std::string
        {
            try
            {
                if (path.has_parent_path())
                {
                    std::filesystem::create_directories(path.parent_path());
                }
                WriteRgba8Png(path, width, height, canvas);
                return {};
            }
            catch (const std::exception& error)
            {
                return error.what();
            }
        });
}

bool EditorRenderBackendBase::PhotoTileDone(PhotoInProgress& photo)
{
    PhotoStatus& status = State().photoStatus;
    // The report is of a frame the render thread drew a frame or two ago: only one of this tile counts.
    const bool report = m_photoViewReport.has_value() && m_photoViewReport->tile == photo.tile;
    if (report)
    {
        const PhotoViewReport& view = *m_photoViewReport;
        status.resolve = view.dlss == DlssMode::Off ? "TAA" : (view.rayReconstruction ? "DLSS ray reconstruction" : "DLSS");
        status.samples = view.offline.has_value() ? view.offline->samples : 0u;
    }
    if (!photo.request.offlinePathTracing)
    {
        return photo.tileFrames >= photo.request.warmupFrames;
    }
    // Path traced: its samples in, then the warm-up frames for the resolve to settle on them. Where
    // the path tracer does not run (no ray queries, the ray scene still building), the rasterised
    // picture after the warm-up frames.
    if (report && m_photoViewReport->offline.has_value() && m_photoViewReport->offline->done)
    {
        ++photo.settledFrames;
    }
    if (photo.settledFrames >= photo.request.warmupFrames)
    {
        return true;
    }
    if (report && !m_photoViewReport->offline.has_value() && photo.tileFrames >= photo.request.warmupFrames)
    {
        LOG_WARN("Photo: tile {} is not path traced (no ray queries, or the ray scene is not ready)", photo.tile + 1);
        return true;
    }
    if (photo.tileFrames >= PhotoPathTraceFrameLimit(photo.request.samplesPerPixel, photo.request.targetSamples, photo.request.warmupFrames))
    {
        LOG_WARN(
            "Photo: tile {} stopped at {} of {} samples; the scene never stood still (moving objects, the sun, a new stream)",
            photo.tile + 1,
            status.samples,
            photo.request.targetSamples);
        return true;
    }
    return false;
}

void EditorRenderBackendBase::WaitForPhotoWrite()
{
    if (m_photo.has_value() && m_photo->writing.has_value())
    {
        m_photo->writing->wait();
        AdvancePhoto();
    }
}

std::optional<SceneCaptureView> EditorRenderBackendBase::PlacePhotoView()
{
    if (!m_photo.has_value() || m_photo->writing.has_value() || m_photo->tile >= m_photo->tiling.tiles.size())
    {
        return std::nullopt;
    }
    PhotoInProgress& photo = *m_photo;
    const PhotoTiling& tiling = photo.tiling;
    const RenderExtent whole{photo.request.width, photo.request.height};
    const bool useZeroToOneDepth = UsesZeroToOneDepth(m_backendType);
    const bool invertRenderYAxis = UsesInvertedRenderYAxis(m_backendType);

    // The viewport's camera when the shutter was pressed, framed inside the viewport at the photo's
    // aspect, at the exposure the viewport showed; a tile sees its part of that frustum.
    SceneCaptureView view;
    view.photo = true;
    view.extent = tiling.ViewExtent();
    view.offlinePathTracing = photo.request.offlinePathTracing;
    view.samplesPerPixel = photo.request.samplesPerPixel;
    view.targetSamples = photo.request.targetSamples;
    view.dlssMode = photo.request.dlssMode;
    view.dlssPreset = State().renderDebug.dlssPreset;
    view.dlssRayReconstruction = photo.request.dlssRayReconstruction;
    view.photoTile = static_cast<uint32_t>(photo.tile);
    view.camera = PlacePhotoCamera(photo.camera, photo.viewportAspect, static_cast<float>(whole.width) / static_cast<float>(whole.height));
    view.matrices.view = view.camera.GetViewMatrix();
    view.matrices.projection = view.camera.GetProjectionMatrix(whole, false, useZeroToOneDepth);
    view.matrices.renderProjection =
        view.camera.GetProjectionMatrix(whole, invertRenderYAxis, useZeroToOneDepth, UsesReverseRenderDepth(m_backendType));
    if (tiling.Tiled())
    {
        const PhotoTile& tile = tiling.tiles[photo.tile];
        view.matrices.projection = TileProjection(view.matrices.projection, tiling, tile, false);
        view.matrices.renderProjection = TileProjection(view.matrices.renderProjection, tiling, tile, invertRenderYAxis);
        view.wholeExtent = whole;
        // A new tile is a cut: what the view's histories hold is of the last one.
        view.resetHistory = photo.tileFrames == 0 && photo.tile > 0;
    }
    ++photo.tileFrames;

    PhotoStatus& status = State().photoStatus;
    status.rendering = true;
    const uint32_t tileFrames = PhotoTileFrames(photo.request);
    status.framesRendered = static_cast<uint32_t>(photo.tile) * tileFrames + std::min(photo.tileFrames, tileFrames);
    status.tile = static_cast<uint32_t>(photo.tile) + 1;
    return view;
}

bool EditorRenderBackendBase::StartVideoRecording(const VideoRecordingRequest& request, std::string& error)
{
    // The render thread reads frames back for the recorder while it draws.
    bool started = false;
    RunWithRenderIdle([&]()
                      {
                          started = StartVideoRecordingNow(request, error);
                      });
    return started;
}

bool EditorRenderBackendBase::StartVideoRecordingNow(const VideoRecordingRequest& request, std::string& error)
{
    if (m_videoRecorder)
    {
        error = "A recording is already running";
        return false;
    }
    // The size the scene renders at now, kept until the recording stops: a video has one size, so
    // the viewport panel shows the scene stretched while it is resized.
    const RenderExtent extent = State().fixedViewportExtent.value_or(State().requestedViewportExtent);
    if (!extent.IsValid())
    {
        error = "The viewport has no size yet";
        return false;
    }
    VideoRecordingSettings settings;
    settings.path = request.path;
    settings.width = extent.width;
    settings.height = extent.height;
    settings.framesPerSecond = request.framesPerSecond;
    settings.pacing = request.everyFrame ? VideoPacing::EveryFrame : VideoPacing::RealTime;
    try
    {
        std::filesystem::create_directories(request.path.parent_path());
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    auto recorder = std::make_unique<VideoRecorder>();
    if (!recorder->Start(settings, error))
    {
        return false;
    }
    m_videoRecorder = std::move(recorder);
    m_fixedViewportExtentBeforeRecording = State().fixedViewportExtent;
    State().fixedViewportExtent = extent;
    State().videoRecording = {};
    LOG_INFO(
        "Recording the viewport to '{}' at {}x{}, {} frames a second",
        request.path.string(), extent.width, extent.height, request.framesPerSecond);
    return true;
}

void EditorRenderBackendBase::StopVideoRecording()
{
    if (!m_videoRecorder)
    {
        return;
    }
    RunWithRenderIdle([this]()
                      {
                          StopVideoRecordingNow();
                      });
}

void EditorRenderBackendBase::StopVideoRecordingNow()
{
    if (!m_videoRecorder)
    {
        return;
    }
    try
    {
        FlushVideoFrames();
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("Failed to read the last frames of the recording back: {}", error.what());
    }
    const std::filesystem::path path = m_videoRecorder->GetSettings().path;
    const VideoRecordingStatus status = m_videoRecorder->Stop();
    m_videoRecorder.reset();
    State().fixedViewportExtent = m_fixedViewportExtentBeforeRecording;
    m_fixedViewportExtentBeforeRecording.reset();

    VideoRecordingIndicator& indicator = State().videoRecording;
    indicator = {};
    indicator.messageTime = std::chrono::steady_clock::now();
    if (!status.error.empty())
    {
        indicator.messageIsError = true;
        indicator.message = fmt::format("Recording stopped: {}", status.error);
        LOG_ERROR("The recording to '{}' stopped: {}", path.string(), status.error);
        return;
    }
    const std::string files = status.files.size() > 1 ? fmt::format(" in {} files", status.files.size()) : std::string{};
    indicator.message = fmt::format(
        "Saved {} ({:.1f} s, {}{})", path.filename().string(), status.videoSeconds, FormatMegabytes(status.bytesWritten), files);
    LOG_INFO(
        "Recorded {:.1f} s ({} frames, {} dropped while encoding) to '{}', {}{}",
        status.videoSeconds, status.framesWritten, status.framesDropped, path.string(), FormatMegabytes(status.bytesWritten), files);
}

void EditorRenderBackendBase::ToggleVideoRecordingFromEditor()
{
    if (m_videoRecorder)
    {
        StopVideoRecording();
        return;
    }
    VideoRecordingRequest request;
    // H.264 MP4 where Media Foundation is there to encode it: small, and sites take it as is.
    request.path = BuildCapturePath("recording", Mp4H264Writer::IsSupported() ? ".mp4" : ".avi");
    std::string error;
    if (!StartVideoRecording(request, error))
    {
        VideoRecordingIndicator& indicator = State().videoRecording;
        indicator = {};
        indicator.messageTime = std::chrono::steady_clock::now();
        indicator.messageIsError = true;
        indicator.message = fmt::format("Cannot record: {}", error);
        LOG_ERROR("Failed to start recording to '{}': {}", request.path.string(), error);
    }
}

void EditorRenderBackendBase::UpdateVideoRecording()
{
    if (!m_videoRecorder)
    {
        return;
    }
    const VideoRecordingStatus status = m_videoRecorder->GetStatus();
    if (!status.error.empty())
    {
        StopVideoRecording();
        return;
    }
    VideoRecordingIndicator& indicator = State().videoRecording;
    indicator.active = true;
    indicator.seconds = status.videoSeconds;
    indicator.bytes = status.bytesWritten;
    indicator.droppedFrames = status.framesDropped;
}

bool EditorRenderBackendBase::StartQuadRecording(const VideoRecordingRequest& request, std::string& error)
{
    // The render thread reads the cameras' pictures back for the recorder while it draws.
    bool started = false;
    RunWithRenderIdle([&]()
                      {
                          started = StartQuadRecordingNow(request, error);
                      });
    return started;
}

bool EditorRenderBackendBase::StartQuadRecordingNow(const VideoRecordingRequest& request, std::string& error)
{
    if (m_quadRecording)
    {
        error = "A quad recording is already running";
        return false;
    }
    if (!FindQuadRecordingTarget().has_value())
    {
        error = "Nothing to film: drive a car or select one";
        return false;
    }
    auto recording = std::make_unique<QuadVideoRecording>();
    recording->settings = ClampQuadRecordingSettings(State().quadRecording);
    recording->mosaic = ComputeQuadMosaic(recording->settings);
    for (size_t index = 0; index < kQuadCameraCount; ++index)
    {
        if (recording->settings.labels)
        {
            recording->labels[index] = QuadCameraSlotName(static_cast<QuadCameraSlot>(index));
        }
    }
    VideoRecordingSettings settings;
    settings.path = request.path;
    settings.width = recording->mosaic.width;
    settings.height = recording->mosaic.height;
    settings.framesPerSecond = request.framesPerSecond;
    settings.pacing = request.everyFrame ? VideoPacing::EveryFrame : VideoPacing::RealTime;
    try
    {
        std::filesystem::create_directories(request.path.parent_path());
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    recording->recorder = std::make_unique<VideoRecorder>();
    if (!recording->recorder->Start(settings, error))
    {
        return false;
    }
    m_quadRecording = std::move(recording);
    State().quadRecordingIndicator = {};
    LOG_INFO(
        "Recording four cameras to '{}' at {}x{}, {} frames a second",
        request.path.string(), settings.width, settings.height, request.framesPerSecond);
    return true;
}

void EditorRenderBackendBase::StopQuadRecording()
{
    if (!m_quadRecording)
    {
        return;
    }
    RunWithRenderIdle([this]()
                      {
                          StopQuadRecordingNow();
                      });
}

void EditorRenderBackendBase::StopQuadRecordingNow()
{
    if (!m_quadRecording)
    {
        return;
    }
    try
    {
        FlushQuadVideoFrames();
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("Failed to read the last frames of the quad recording back: {}", error.what());
    }
    const std::filesystem::path path = m_quadRecording->recorder->GetSettings().path;
    const VideoRecordingStatus status = m_quadRecording->recorder->Stop();
    m_quadRecording.reset();

    VideoRecordingIndicator& indicator = State().quadRecordingIndicator;
    indicator = {};
    indicator.messageTime = std::chrono::steady_clock::now();
    if (!status.error.empty())
    {
        indicator.messageIsError = true;
        indicator.message = fmt::format("Recording stopped: {}", status.error);
        LOG_ERROR("The quad recording to '{}' stopped: {}", path.string(), status.error);
        return;
    }
    const std::string files = status.files.size() > 1 ? fmt::format(" in {} files", status.files.size()) : std::string{};
    indicator.message = fmt::format(
        "Saved {} ({:.1f} s, {}{})", path.filename().string(), status.videoSeconds, FormatMegabytes(status.bytesWritten), files);
    LOG_INFO(
        "Recorded four cameras for {:.1f} s ({} frames, {} dropped while encoding) to '{}', {}{}",
        status.videoSeconds, status.framesWritten, status.framesDropped, path.string(), FormatMegabytes(status.bytesWritten), files);
}

void EditorRenderBackendBase::ToggleQuadRecordingFromEditor()
{
    if (m_quadRecording)
    {
        StopQuadRecording();
        return;
    }
    VideoRecordingRequest request;
    request.path = BuildCapturePath("quad", Mp4H264Writer::IsSupported() ? ".mp4" : ".avi");
    request.framesPerSecond = ClampQuadRecordingSettings(State().quadRecording).framesPerSecond;
    std::string error;
    if (!StartQuadRecording(request, error))
    {
        VideoRecordingIndicator& indicator = State().quadRecordingIndicator;
        indicator = {};
        indicator.messageTime = std::chrono::steady_clock::now();
        indicator.messageIsError = true;
        indicator.message = fmt::format("Cannot record: {}", error);
        LOG_ERROR("Failed to start the quad recording to '{}': {}", request.path.string(), error);
    }
}

void EditorRenderBackendBase::UpdateQuadRecording()
{
    if (!m_quadRecording)
    {
        return;
    }
    const VideoRecordingStatus status = m_quadRecording->recorder->GetStatus();
    if (!status.error.empty())
    {
        StopQuadRecording();
        return;
    }
    VideoRecordingIndicator& indicator = State().quadRecordingIndicator;
    indicator.active = true;
    indicator.seconds = status.videoSeconds;
    indicator.bytes = status.bytesWritten;
    indicator.droppedFrames = status.framesDropped;
}

std::optional<EditorRenderBackendBase::QuadRecordingTarget> EditorRenderBackendBase::FindQuadRecordingTarget()
{
    const VehicleDriveStatus drive = VehicleDriveService::GetStatus(State());
    if (drive.active)
    {
        return QuadRecordingTarget{drive.pose, drive.vehicleName};
    }
    const IEditorWorld& world = EditorWorld();
    if (!world.HasSelection() || !world.HasModelComponent(world.GetSelectedEntity()))
    {
        return std::nullopt;
    }
    // A car that is not driven faces along its model's +Z, as a car's model is made.
    const entt::entity entity = world.GetSelectedEntity();
    const glm::mat4 model = world.GetModelMatrix(entity);
    PhysicsPose pose;
    pose.position = glm::dvec3(glm::vec3(model[3]));
    const glm::mat3 axes(glm::normalize(glm::vec3(model[0])), glm::normalize(glm::vec3(model[1])), glm::normalize(glm::vec3(model[2])));
    pose.rotation = glm::normalize(glm::quat_cast(axes));
    return QuadRecordingTarget{pose, world.GetTag(entity).name};
}

void EditorRenderBackendBase::UpdateCaptureViews()
{
    // The frame before this one may have been a photo tile's last: take it before the views are
    // placed again.
    AdvancePhoto();
    m_captureViews.clear();
    State().quadRecordingTarget.clear();
    const std::optional<QuadRecordingTarget> target = FindQuadRecordingTarget();
    if (target.has_value())
    {
        State().quadRecordingTarget = target->name;
    }
    const bool useZeroToOneDepth = UsesZeroToOneDepth(m_backendType);
    const bool invertRenderYAxis = UsesInvertedRenderYAxis(m_backendType);
    const auto setMatrices = [&](SceneCaptureView& view)
    {
        view.matrices.view = view.camera.GetViewMatrix();
        view.matrices.projection = view.camera.GetProjectionMatrix(view.extent, false, useZeroToOneDepth);
        view.matrices.renderProjection =
            view.camera.GetProjectionMatrix(view.extent, invertRenderYAxis, useZeroToOneDepth, UsesReverseRenderDepth(m_backendType));
    };
    const bool quadCameras = target.has_value() && (m_quadRecording || State().quadRecordingPreview);
    // The pictures keep the sizes a recording started with; where the cameras are follows the
    // window as it is edited.
    const QuadRecordingSettings live = ClampQuadRecordingSettings(State().quadRecording);
    for (size_t index = 0; quadCameras && index < kQuadCameraCount; ++index)
    {
        QuadCameraSettings camera = live.cameras[index];
        if (m_quadRecording)
        {
            camera.width = m_quadRecording->settings.cameras[index].width;
            camera.height = m_quadRecording->settings.cameras[index].height;
        }
        SceneCaptureView view;
        view.extent = RenderExtent{camera.width, camera.height};
        view.camera = PlaceQuadCamera(State().camera, target->pose, camera);
        setMatrices(view);
        m_captureViews.push_back(view);
    }

    // The photo's view, last.
    if (std::optional<SceneCaptureView> photo = PlacePhotoView())
    {
        m_captureViews.push_back(*photo);
    }
}

void EditorRenderBackendBase::UpdateKhronosReferenceFraming(RenderExtent extent)
{
    if (!State().renderDebug.khronosReference || extent.width == 0 || extent.height == 0)
    {
        m_khronosReferenceFraming.reset();
        return;
    }

    // The whole scene, measured as the viewer measures it (ComputeKhronosViewerExtents). Models still
    // loading are left out; the view reframes once they arrive.
    bool any = false;
    glm::vec3 sceneMin(0.0f);
    glm::vec3 sceneMax(0.0f);
    const IEditorWorld& world = EditorWorld();
    for (const entt::entity entity : world.Registry().view<const ModelComponent>())
    {
        const std::string& sourcePath = world.GetModel(entity).sourcePath;
        const std::shared_ptr<const LoadedModelData> model = sourcePath.empty() ? nullptr : ModelCache::Get(sourcePath);
        glm::vec3 minBounds;
        glm::vec3 maxBounds;
        if (!model || !ComputeKhronosViewerExtents(*model, world.GetModelMatrix(entity), minBounds, maxBounds))
        {
            continue;
        }
        sceneMin = any ? glm::min(sceneMin, minBounds) : minBounds;
        sceneMax = any ? glm::max(sceneMax, maxBounds) : maxBounds;
        any = true;
    }
    if (!any)
    {
        return;
    }

    const KhronosReferenceFraming framing{
        sceneMin,
        sceneMax,
        static_cast<float>(extent.width) / static_cast<float>(extent.height)};
    if (m_khronosReferenceFraming != framing)
    {
        State().camera.FrameBoundsLikeKhronosViewer(framing.minBounds, framing.maxBounds, framing.aspectRatio);
        m_khronosReferenceFraming = framing;
    }
}

void EditorRenderBackendBase::UpdateViewportMatrices(RenderExtent extent)
{
    UpdateKhronosReferenceFraming(extent);
    if (extent.width > 0 && extent.height > 0)
    {
        m_viewportAspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    }
    const bool useZeroToOneDepth = UsesZeroToOneDepth(m_backendType);
    const bool invertRenderYAxis = UsesInvertedRenderYAxis(m_backendType);
    State().viewportMatrices.view = State().camera.GetViewMatrix();
    State().viewportMatrices.projection = State().camera.GetProjectionMatrix(extent, false, useZeroToOneDepth);
    State().viewportMatrices.renderProjection =
        State().camera.GetProjectionMatrix(extent, invertRenderYAxis, useZeroToOneDepth, UsesReverseRenderDepth(m_backendType));
    State().viewportMatrices.model =
        EditorWorld().HasSelection() ? EditorWorld().GetModelMatrix(EditorWorld().GetSelectedEntity()) : glm::mat4(1.0f);
}

EditorUiFrameResult EditorRenderBackendBase::DrawEditorUi(ImTextureID viewportTextureId, RenderExtent viewportExtent)
{
    const bool selectionIsModel =
        EditorWorld().HasSelection() &&
        !EditorWorld().HasLightComponent(EditorWorld().GetSelectedEntity());
    const std::string selectedModelPath =
        selectionIsModel ? EditorWorld().GetSelectedModel().sourcePath : std::string{};

    State().editorUi.SetVehicleDriveStatus(VehicleDriveService::GetStatus(State()));
    State().editorUi.SetVehicleRigStatus(VehicleRigService::GetStatus(State()));
    State().editorUi.SetDriverProblems(State().vehicleDrivers.problems);
    State().editorUi.SetDriverGrips(State().vehicleDrivers.grips);
    State().editorUi.SetVideoRecordingStatus(State().videoRecording);
    State().editorUi.SetQuadRecordingStatus(State().quadRecordingIndicator, State().quadRecordingTarget);
    State().editorUi.SetPhotoStatus(State().photoStatus);
    State().editorUi.SetForcedViewportExtent(State().fixedViewportExtent);
    State().editorUi.SetAudioStatus(State().audioStatus);
    State().editorUi.SetProcessStatus(State().processStatus);
    EditorUiFrameResult result = State().editorUi.Draw(
        State().camera,
        State().viewportMatrices,
        EditorWorld(),
        selectedModelPath,
        State().lastModelLoadError,
        State().lastSceneIoError,
        State().sceneUploadStatus,
        viewportTextureId,
        viewportExtent,
        m_backendType);

    // A slider or color drag changes the settings every frame; write the file once the drag ends
    // rather than on each of those frames.
    if (result.engineSettingsChanged)
    {
        State().engineSettingsDirty = true;
    }
    // The camera and the renderer are edited in place by the panels, the menus and the mouse wheel:
    // compare them with what was saved rather than flag each of those edits.
    if (!State().viewSettingsFromCommandLine &&
        UpdateEngineViewSettings(State().engineSettings.view, State().camera, result.renderDebug))
    {
        State().engineSettingsDirty = true;
    }
    if ((State().engineSettingsDirty || State().engineSettingsNeedsBootstrapSave) && !ImGui::IsAnyItemActive())
    {
        State().editorUi.WriteEngineSettings(State().engineSettings);
        SaveEngineSettings();
    }

    // Loading overlay — drawn on top of all other windows.
    const bool importing = State().asyncImport.IsLoading();
    if (State().asyncLoad.IsLoading() || State().asyncSceneLoad.IsLoading() || importing)
    {
        const bool loadingScene = State().asyncSceneLoad.IsLoading();
        // A model or scene load takes precedence: the overlay shows one task at a time.
        const bool showImport = importing && !loadingScene && !State().asyncLoad.IsLoading();
        const std::string& activePath = showImport      ? State().asyncImport.sourcePath
                                        : loadingScene ? State().asyncSceneLoad.path
                                                       : State().asyncLoad.path;
        const char* label = showImport ? "Importing" : loadingScene ? "Loading Scene" : "Loading";

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::SetNextWindowSize(ImVec2(360.0f * State().editorUi.GetEffectiveUiScale(), 0.0f));
        if (ImGui::Begin("##async_load_overlay", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize))
        {
            const std::string filename = std::filesystem::path(activePath).filename().string();

            const char* kSpinner = "|/-\\";
            const int spinFrame = static_cast<int>(ImGui::GetTime() * 10.0) & 3;
            ImGui::Text("%c  %s: %s", kSpinner[spinFrame], label, filename.c_str());

            ImGui::Spacing();
            const float fraction = showImport      ? State().asyncImport.Progress()
                                   : loadingScene ? State().asyncSceneLoad.Progress()
                                                  : State().asyncLoad.Progress();
            char progressText[16];
            std::snprintf(progressText, sizeof(progressText), "%.0f%%", fraction * 100.0f);
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), progressText);
            ImGui::Spacing();

            ImGui::TextDisabled("Editor remains interactive during loading...");
        }
        ImGui::End();
    }

    return result;
}

bool EditorRenderBackendBase::HasDrawableArea() const
{
    // Windows keeps a minimized window's last size, but its surface has none.
    if ((SDL_GetWindowFlags(m_window.GetSDLWindow()) & SDL_WINDOW_MINIMIZED) != 0)
    {
        return false;
    }

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(m_window.GetSDLWindow(), &width, &height))
    {
        throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
    }

    return width > 0 && height > 0;
}

RendererSharedState& EditorRenderBackendBase::State()
{
    // The main thread changes it while the render thread draws: render work reads its frame packet.
    assert(!RenderThread::IsRenderWork() && "render work must not read the editor's shared state");
    return *m_sharedState;
}

const RendererSharedState& EditorRenderBackendBase::State() const
{
    assert(!RenderThread::IsRenderWork() && "render work must not read the editor's shared state");
    return *m_sharedState;
}

IEditorWorld& EditorRenderBackendBase::EditorWorld()
{
    return State().GetEditorWorld();
}

RendererWorld& EditorRenderBackendBase::RenderWorld()
{
    return State().rendererWorld;
}

Window& EditorRenderBackendBase::GetWindow() const
{
    return m_window;
}

std::optional<glm::vec3> EditorRenderBackendBase::SelectionOrbitPivot()
{
    const IEditorWorld& world = EditorWorld();
    if (!world.HasSelection())
    {
        return std::nullopt;
    }
    const entt::entity entity = world.GetSelectedEntity();
    glm::vec3 minBounds{};
    glm::vec3 maxBounds{};
    if (ComputeWorldModelBounds(world, entity, minBounds, maxBounds))
    {
        return (minBounds + maxBounds) * 0.5f;
    }
    return glm::vec3(world.GetModelMatrix(entity)[3]);
}

void EditorRenderBackendBase::UpdateCameraFromInput(
    Camera& camera,
    const InputState& input,
    float deltaTime,
    bool blockKeyboardInput,
    const std::optional<glm::vec3>& orbitPivot)
{
    const float moveDistance = camera.moveSpeed * deltaTime;
    const bool mousePanActive = input.IsMousePanActive();

    if (!blockKeyboardInput && !mousePanActive)
    {
        if (input.IsKeyDown(KeyCodes::W))
        {
            camera.MoveForward(moveDistance);
        }
        if (input.IsKeyDown(KeyCodes::S))
        {
            camera.MoveForward(-moveDistance);
        }
        if (input.IsKeyDown(KeyCodes::A))
        {
            camera.MoveRight(-moveDistance);
        }
        if (input.IsKeyDown(KeyCodes::D))
        {
            camera.MoveRight(moveDistance);
        }
    }

    if (!blockKeyboardInput)
    {
        const int gamepadIndex = input.GetFirstConnectedGamepadIndex();
        if (gamepadIndex >= 0)
        {
            const uint32_t playerIndex = static_cast<uint32_t>(gamepadIndex);
            const float leftStickX = input.GetGamepadAxis(GamepadAxis::LeftX, playerIndex);
            const float leftStickY = input.GetGamepadAxis(GamepadAxis::LeftY, playerIndex);
            const float leftTrigger = input.GetGamepadAxis(GamepadAxis::LeftTrigger, playerIndex);
            const float rightTrigger = input.GetGamepadAxis(GamepadAxis::RightTrigger, playerIndex);

            camera.MoveForward(-leftStickY * moveDistance);
            camera.MoveRight(leftStickX * moveDistance);
            camera.MoveUp((rightTrigger - leftTrigger) * moveDistance);

            const float rightStickX = input.GetGamepadAxis(GamepadAxis::RightX, playerIndex);
            const float rightStickY = input.GetGamepadAxis(GamepadAxis::RightY, playerIndex);
            if (std::abs(rightStickX) > 0.0f || std::abs(rightStickY) > 0.0f)
            {
                const float gamepadLookSpeed = 180.0f * deltaTime;
                camera.Rotate(
                    rightStickX * gamepadLookSpeed,
                    -rightStickY * gamepadLookSpeed);
            }
        }
    }

    if (input.IsMouseLookActive())
    {
        const float deltaYaw = input.GetMouseDeltaX() * camera.mouseSensitivity;
        const float deltaPitch = -input.GetMouseDeltaY() * camera.mouseSensitivity;
        const bool altHeld = input.IsKeyDown(KeyCodes::LeftAlt) || input.IsKeyDown(KeyCodes::RightAlt);
        if (altHeld && orbitPivot.has_value())
        {
            camera.Orbit(*orbitPivot, deltaYaw, deltaPitch);
        }
        else
        {
            camera.Rotate(deltaYaw, deltaPitch);
        }
    }

    if (input.IsMousePanActive())
    {
        const float panDistancePerPixel = moveDistance * 0.1f;
        camera.MoveRight(-input.GetMouseDeltaX() * panDistancePerPixel);
        camera.MoveUp(input.GetMouseDeltaY() * panDistancePerPixel);
    }

    const float wheelDelta = input.GetMouseWheelDelta();
    if (wheelDelta != 0.0f)
    {
        if (input.IsMouseLookActive())
        {
            const float speedScalePerNotch = 1.2f;
            camera.moveSpeed = std::clamp(
                camera.moveSpeed * std::pow(speedScalePerNotch, wheelDelta),
                WorldUnits::kUiCameraMoveSpeedMinMetersPerSecond,
                WorldUnits::kUiCameraMoveSpeedMaxMetersPerSecond);
        }
        else
        {
            const float fovDegreesPerNotch = 2.0f;
            camera.fovDegrees = std::clamp(
                camera.fovDegrees - wheelDelta * fovDegreesPerNotch,
                WorldUnits::kUiCameraFovMinDegrees,
                WorldUnits::kUiCameraFovMaxDegrees);
        }
    }
}

// =============================================================================
// [EDITOR] Initialization & settings persistence
// =============================================================================

void EditorRenderBackendBase::EnsureInitialized(std::optional<std::string> startupModelPath)
{
    if (State().initialized)
    {
        State().lastFrameTime = std::chrono::steady_clock::now();
        return;
    }

    State().engineSettingsPath = BuildEngineSettingsPath();
    State().engineSettingsNeedsBootstrapSave = !std::filesystem::exists(State().engineSettingsPath);
    if (!LoadEngineSettings(State().engineSettingsPath, State().engineSettings, State().lastEngineSettingsError))
    {
        LOG_ERROR(
            "Failed to load engine settings '{}': {}",
            State().engineSettingsPath.string(),
            State().lastEngineSettingsError);
        State().engineSettingsNeedsBootstrapSave = true;
    }
    else
    {
        State().lastEngineSettingsError.clear();
    }
    if (!State().viewSettingsFromCommandLine)
    {
        ApplyEngineViewSettings(State().engineSettings.view, State().camera, State().editorUi.EditRenderDebug());
    }

    // Deterministic first scan of the asset tree (uuid sidecars, duplicate
    // cleanup) before anything resolves asset references.
    AssetRegistry::Initialize(EnginePaths::AssetsRoot());

    State().editorWorld = CreateEditorWorld();
    RenderWorld().SetSceneWorld(EditorWorld());
    InitializeEditorScene();
    EditorWorld().CreateTwoCubeTestScene();
    if (startupModelPath.has_value())
    {
        State().pendingModelLoads.push_back({*startupModelPath, false});
    }

    RebuildSceneRenderables(State());
    State().initialized = true;
    State().renderablesDirty = true;
    State().lastFrameTime = std::chrono::steady_clock::now();
}

void EditorRenderBackendBase::InitializeEditorScene()
{
    EditorWorld().LoadConfig((EnginePaths::AssetsRoot() / "editor" / "default_scene.yaml").string());
}

void EditorRenderBackendBase::SaveEngineSettings()
{
    if (State().engineSettingsPath.empty())
    {
        State().engineSettingsPath = BuildEngineSettingsPath();
    }

    std::string errorMessage;
    // Qualified to pick the free function over the enclosing class's member of
    // the same name.
    if (!me::SaveEngineSettings(State().engineSettingsPath, State().engineSettings, errorMessage))
    {
        State().lastEngineSettingsError = errorMessage;
        LOG_ERROR(
            "Failed to save engine settings '{}': {}",
            State().engineSettingsPath.string(),
            errorMessage);
        return;
    }

    State().engineSettingsNeedsBootstrapSave = false;
    State().engineSettingsDirty = false;
    State().lastEngineSettingsError.clear();
}
}
