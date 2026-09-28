#include "editor_backend_base.h"

#include "services/capture_state.h"
#include "services/entity_edit_service.h"
#include "services/model_import_service.h"
#include "services/scene_io_service.h"
#include "services/scene_renderables.h"

#include <engine/asset/asset_registry.h>
#include <engine/asset/kn5_importer.h>
#include <engine/asset/model_cache.h>
#include <engine/asset/model_loader.h>
#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/platform/window/window.h>
#include <engine/scene/world_units.h>

#include <algorithm>
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
}

bool EditorRenderBackendBase::TickSharedFrame()
{
    const auto currentFrameTime = std::chrono::steady_clock::now();
    const float deltaTime = std::chrono::duration<float>(currentFrameTime - State().lastFrameTime).count();
    State().lastFrameTime = currentFrameTime;
    State().frameDeltaSeconds = deltaTime;

    State().input.Update();
    UpdateCameraFromInput(State().camera, State().input, deltaTime, WantsKeyboardCapture());
    State().input.EndFrame();

    return HasDrawableArea();
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
            // Assetto Corsa files are never loaded as they are: their import converts them.
            if (!AssetRegistry::IsUnderAssetsRoot(path) || Kn5Importer::IsKn5Path(path) ||
                Kn5Importer::IsLayoutPath(path))
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
    State().input.SetViewportInteractionRegion(
        uiFrame.viewportInteractionRect,
        uiFrame.viewportAllowsMouseInteraction);

    const EditorUiActions& actions = uiFrame.actions;
    std::string& modelError = State().lastModelLoadError;
    std::string& sceneError = State().lastSceneIoError;

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
                        ModelImportService::UpdateImportedModelMaterialDefinitions(State(), update->modelPath, update->materials);
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
    if (const auto& savePath = actions.selectedSceneSavePath)
    {
        RunUiAction(sceneError, fmt::format("save scene '{}'", *savePath), [&]
                    {
                        SceneIoService::SaveScene(State(), *savePath);
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

// The viewport as it is on screen, before this frame records, to
// captures/viewport_<local date>_<time>.png, with what it takes to render it again beside it:
// viewport_<...>.scene.yaml and viewport_<...>.state.yaml, replayed with miniengine_app --state.
void EditorRenderBackendBase::CaptureViewportWithState()
{
    SDL_DateTime now{};
    SDL_Time ticks = 0;
    if (!SDL_GetCurrentTime(&ticks) || !SDL_TimeToDateTime(ticks, &now, true))
    {
        now = SDL_DateTime{};
    }
    char name[64];
    std::snprintf(
        name, sizeof(name), "viewport_%04d%02d%02d_%02d%02d%02d.png",
        now.year, now.month, now.day, now.hour, now.minute, now.second);
    const std::filesystem::path path = EnginePaths::ProjectRoot() / "captures" / name;
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
    if ((State().engineSettingsDirty || State().engineSettingsNeedsBootstrapSave) && !ImGui::IsAnyItemActive())
    {
        State().editorUi.WriteEngineSettings(State().engineSettings);
        SaveEngineSettings();
    }

    // Loading overlay — drawn on top of all other windows.
    if (State().asyncLoad.IsLoading() || State().asyncSceneLoad.IsLoading())
    {
        const bool loadingScene = State().asyncSceneLoad.IsLoading();
        const std::string& activePath = loadingScene ? State().asyncSceneLoad.path : State().asyncLoad.path;
        const char* label = loadingScene ? "Loading Scene" : "Loading";

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
            const float fraction =
                loadingScene ? State().asyncSceneLoad.Progress() : State().asyncLoad.Progress();
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
    return *m_sharedState;
}

const RendererSharedState& EditorRenderBackendBase::State() const
{
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

void EditorRenderBackendBase::UpdateCameraFromInput(
    Camera& camera,
    const InputState& input,
    float deltaTime,
    bool blockKeyboardInput)
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
        camera.Rotate(
            input.GetMouseDeltaX() * camera.mouseSensitivity,
            -input.GetMouseDeltaY() * camera.mouseSensitivity);
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
