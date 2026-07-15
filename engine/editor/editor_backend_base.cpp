#include "editor_backend_base.h"

#include "services/entity_edit_service.h"
#include "services/model_import_service.h"
#include "services/scene_io_service.h"
#include "services/scene_renderables.h"

#include <log/log.h>
#include <window/window.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

EditorRenderBackendBase::EditorRenderBackendBase(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    RenderBackendType backendType,
    std::optional<std::string> startupModelPath
)
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

    if (State().pendingModelPath.has_value())
    {
        const std::string path = *State().pendingModelPath;
        State().pendingModelPath.reset();

        try
        {
            LOG_INFO("Loading model: {}", path);
            if (EditorWorld().HasSelection())
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
            LOG_ERROR("Failed to load model '{}': {}", path, error.what());
        }

        State().lastFrameTime = std::chrono::steady_clock::now();
    }

    renderablesDirty |= EntityEditService::PumpAsyncModelLoad(State());
    renderablesDirty |= SceneIoService::PumpAsyncSceneLoad(State());

    State().renderablesDirty = false;
    return renderablesDirty;
}

void EditorRenderBackendBase::ApplyUiActions(const EditorUiFrameResult& uiFrame)
{
    State().requestedViewportExtent = uiFrame.viewportExtent;
    State().input.SetViewportInteractionRegion(
        uiFrame.viewportInteractionRect,
        uiFrame.viewportAllowsMouseInteraction
    );

    try
    {
        if (uiFrame.actions.hoveredViewportModel.has_value())
        {
            EntityEditService::UpdateViewportModelPreview(
                State(),
                uiFrame.actions.hoveredViewportModel->modelPath,
                uiFrame.actions.hoveredViewportModel->worldPosition
            );
        }
        else
        {
            EntityEditService::ClearViewportModelPreview(State());
        }
    }
    catch (const std::exception& error)
    {
        State().lastModelLoadError = error.what();
        LOG_ERROR("Failed to update viewport model preview: {}", error.what());
    }

    if (uiFrame.actions.importedModelRequest.has_value())
    {
        try
        {
            ModelImportService::ImportModelIntoAssetDirectory(
                uiFrame.actions.importedModelRequest->sourcePath,
                uiFrame.actions.importedModelRequest->destinationDirectory
            );
            State().lastModelLoadError.clear();
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR(
                "Failed to import model '{}' into '{}': {}",
                uiFrame.actions.importedModelRequest->sourcePath,
                uiFrame.actions.importedModelRequest->destinationDirectory,
                error.what()
            );
        }
    }
    if (uiFrame.actions.selectedModelPath.has_value())
    {
        State().pendingModelPath = *uiFrame.actions.selectedModelPath;
    }
    if (uiFrame.actions.createSceneEntity)
    {
        try
        {
            EntityEditService::CreateSceneEntity(State());
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to create scene entity: {}", error.what());
        }
    }
    if (uiFrame.actions.createLightEntity.has_value())
    {
        try
        {
            EntityEditService::CreateSceneLightEntity(
                State(),
                uiFrame.actions.createLightEntity->name,
                uiFrame.actions.createLightEntity->type
            );
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to create light entity: {}", error.what());
        }
    }
    if (uiFrame.actions.deleteSelectedSceneEntity)
    {
        try
        {
            if (EditorWorld().HasSelection() &&
                EditorWorld().HasLightComponent(EditorWorld().GetSelectedEntity()))
            {
                EntityEditService::DeleteSelectedLightEntity(State());
            }
            else
            {
                EntityEditService::DeleteSelectedSceneEntity(State());
            }
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to delete selected entity: {}", error.what());
        }
    }
    if (uiFrame.actions.droppedViewportModel.has_value())
    {
        try
        {
            EntityEditService::CommitViewportModelPreview(
                State(),
                uiFrame.actions.droppedViewportModel->modelPath,
                uiFrame.actions.droppedViewportModel->worldPosition
            );
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR(
                "Failed to place dropped model '{}' into scene: {}",
                uiFrame.actions.droppedViewportModel->modelPath,
                error.what()
            );
        }
    }
    if (uiFrame.actions.updatedImportedMaterial.has_value())
    {
        try
        {
            ModelImportService::UpdateImportedMaterialDefinition(
                State(),
                uiFrame.actions.updatedImportedMaterial->modelPath,
                uiFrame.actions.updatedImportedMaterial->materialIndex,
                uiFrame.actions.updatedImportedMaterial->material
            );
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR(
                "Failed to update imported material '{}' index {}: {}",
                uiFrame.actions.updatedImportedMaterial->modelPath,
                uiFrame.actions.updatedImportedMaterial->materialIndex,
                error.what()
            );
        }
    }
    if (uiFrame.actions.updatedImportedModelMaterials.has_value())
    {
        try
        {
            ModelImportService::UpdateImportedModelMaterialDefinitions(
                State(),
                uiFrame.actions.updatedImportedModelMaterials->modelPath,
                uiFrame.actions.updatedImportedModelMaterials->materials
            );
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR(
                "Failed to update imported model materials '{}': {}",
                uiFrame.actions.updatedImportedModelMaterials->modelPath,
                error.what()
            );
        }
    }
    if (uiFrame.actions.selectedBaseColorTexturePath.has_value())
    {
        try
        {
            EntityEditService::ApplySelectedModelBaseColorTexture(
                State(),
                *uiFrame.actions.selectedBaseColorTexturePath
            );
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR(
                "Failed to apply base color texture '{}' to selected model: {}",
                *uiFrame.actions.selectedBaseColorTexturePath,
                error.what()
            );
        }
    }
    if (uiFrame.actions.clearSelectedBaseColorTexture)
    {
        try
        {
            EntityEditService::ClearSelectedModelBaseColorTexture(State());
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to clear selected model texture override: {}", error.what());
        }
    }
    if (uiFrame.actions.selectedSceneLoadPath.has_value())
    {
        State().pendingScenePath = *uiFrame.actions.selectedSceneLoadPath;
    }
    if (uiFrame.actions.selectedSceneSavePath.has_value())
    {
        try
        {
            SceneIoService::SaveScene(State(), *uiFrame.actions.selectedSceneSavePath);
        }
        catch (const std::exception& error)
        {
            State().lastSceneIoError = error.what();
            LOG_ERROR("Failed to save scene '{}': {}", *uiFrame.actions.selectedSceneSavePath, error.what());
        }
    }
    for (const std::string& deletePath : uiFrame.actions.deleteAssetPaths)
    {
        try
        {
            ModelImportService::DeleteAssetPath(deletePath);
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to delete asset '{}': {}", deletePath, error.what());
        }
    }
    if (uiFrame.actions.pastedAsset.has_value())
    {
        try
        {
            ModelImportService::PasteAsset(
                uiFrame.actions.pastedAsset->sourcePath,
                uiFrame.actions.pastedAsset->destinationDirectory
            );
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR(
                "Failed to paste asset '{}' into '{}': {}",
                uiFrame.actions.pastedAsset->sourcePath,
                uiFrame.actions.pastedAsset->destinationDirectory,
                error.what()
            );
        }
    }
}

void EditorRenderBackendBase::UpdateViewportMatrices(RenderExtent extent)
{
    const bool useZeroToOneDepth = UsesZeroToOneDepth(m_backendType);
    const bool invertRenderYAxis = UsesInvertedRenderYAxis(m_backendType);
    State().viewportMatrices.view = State().camera.GetViewMatrix();
    State().viewportMatrices.projection = State().camera.GetProjectionMatrix(extent, false, useZeroToOneDepth);
    State().viewportMatrices.renderProjection =
        State().camera.GetProjectionMatrix(extent, invertRenderYAxis, useZeroToOneDepth);
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
        viewportTextureId,
        viewportExtent,
        m_backendType
    );

    if (result.engineSettingsChanged || State().engineSettingsNeedsBootstrapSave)
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
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f));
        if (ImGui::Begin("##async_load_overlay", nullptr,
            ImGuiWindowFlags_NoDecoration  | ImGuiWindowFlags_NoInputs     |
            ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav         | ImGuiWindowFlags_AlwaysAutoResize))
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

const IEditorWorld& EditorRenderBackendBase::EditorWorld() const
{
    return State().GetEditorWorld();
}

RendererWorld& EditorRenderBackendBase::RenderWorld()
{
    return State().rendererWorld;
}

const RendererWorld& EditorRenderBackendBase::RenderWorld() const
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
    bool blockKeyboardInput
)
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
                    -rightStickY * gamepadLookSpeed
                );
            }
        }
    }

    if (input.IsMouseLookActive())
    {
        camera.Rotate(
            input.GetMouseDeltaX() * camera.mouseSensitivity,
            -input.GetMouseDeltaY() * camera.mouseSensitivity
        );
    }

    if (input.IsMousePanActive())
    {
        const float panDistancePerPixel = moveDistance * 0.1f;
        camera.MoveRight(-input.GetMouseDeltaX() * panDistancePerPixel);
        camera.MoveUp(input.GetMouseDeltaY() * panDistancePerPixel);
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
            State().lastEngineSettingsError
        );
        State().engineSettingsNeedsBootstrapSave = true;
    }
    else
    {
        State().lastEngineSettingsError.clear();
    }

    State().editorWorld = CreateEditorWorld();
    RenderWorld().SetSceneWorld(EditorWorld());
    InitializeEditorScene();
    EditorWorld().CreateTwoCubeTestScene();
    if (startupModelPath.has_value())
    {
        State().pendingModelPath = *startupModelPath;
    }

    RebuildSceneRenderables(State());
    State().initialized = true;
    State().renderablesDirty = true;
    State().lastFrameTime = std::chrono::steady_clock::now();
}

void EditorRenderBackendBase::InitializeEditorScene()
{
    EditorWorld().LoadConfig(MINIENGINE_ASSET_DIR "/editor/default_scene.yaml");
}

void EditorRenderBackendBase::SaveEngineSettings()
{
    if (State().engineSettingsPath.empty())
    {
        State().engineSettingsPath = BuildEngineSettingsPath();
    }

    std::string errorMessage;
    if (!::SaveEngineSettings(State().engineSettingsPath, State().engineSettings, errorMessage))
    {
        State().lastEngineSettingsError = errorMessage;
        LOG_ERROR(
            "Failed to save engine settings '{}': {}",
            State().engineSettingsPath.string(),
            errorMessage
        );
        return;
    }

    State().engineSettingsNeedsBootstrapSave = false;
    State().lastEngineSettingsError.clear();
}
