#include "editor_backend_base.h"

#include "services/capture_state.h"
#include "services/entity_edit_service.h"
#include "services/model_import_service.h"
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
#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/core/threading/render_thread.h>
#include <engine/logic/world_bounds.h>
#include <engine/platform/window/window.h>
#include <engine/scene/world_units.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace me
{

namespace
{
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
    const auto currentFrameTime = std::chrono::steady_clock::now();
    const float deltaTime = std::chrono::duration<float>(currentFrameTime - State().lastFrameTime).count();
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
    if (State().fixedViewportExtent.has_value())
    {
        // The viewport panel built its matrices for its own size; the scene renders at the fixed one.
        UpdateViewportMatrices(*State().fixedViewportExtent);
    }
    State().renderDebug = uiFrame.renderDebug;
    if (AudioEngine* const audio = State().audio.get())
    {
        audio->SetMasterVolume(uiFrame.audio.EffectiveVolume());
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
    if (actions.pauseVehicleDrive.has_value())
    {
        VehicleDriveService::SetPaused(State(), *actions.pauseVehicleDrive);
    }
    if (actions.brushTyreBristles.has_value())
    {
        VehicleDriveService::SetBrushTyreBristles(State(), (*actions.brushTyreBristles)[0], (*actions.brushTyreBristles)[1]);
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
                        EntityEditService::ApplySelectedModelDriver(State(), driver->vehicleUuid, driver->seatOffset);
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
}

namespace
{
// ProjectRoot()/captures/<prefix>_<local date>_<time><extension>.
std::filesystem::path BuildCapturePath(const char* prefix, const char* extension)
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
    return EnginePaths::ProjectRoot() / "captures" / name;
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
    State().editorUi.SetVideoRecordingStatus(State().videoRecording);
    State().editorUi.SetAudioStatus(State().audioStatus);
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
