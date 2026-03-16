#include "editor_backend_base.h"

#include <log/log.h>
#include <window/window.h>

#include <array>
#include <filesystem>
#include <stdexcept>

namespace
{
MaterialPushConstants BuildDefaultMaterialForTag(const std::string& tagName)
{
    MaterialPushConstants material{};
    if (tagName == "Cube A")
    {
        material.baseColorFactor[0] = 1.0f;
        material.baseColorFactor[1] = 0.55f;
        material.baseColorFactor[2] = 0.35f;
    }
    else if (tagName == "Cube B")
    {
        material.baseColorFactor[0] = 0.35f;
        material.baseColorFactor[1] = 0.75f;
        material.baseColorFactor[2] = 1.0f;
    }
    return material;
}
}

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
}

std::optional<RenderBackendType> EditorRenderBackendBase::ConsumeBackendSwitchRequest()
{
    std::optional<RenderBackendType> request = State().requestedBackendSwitch;
    State().requestedBackendSwitch.reset();
    return request;
}

bool EditorRenderBackendBase::TickSharedFrame()
{
    const auto currentFrameTime = std::chrono::steady_clock::now();
    const float deltaTime = std::chrono::duration<float>(currentFrameTime - State().lastFrameTime).count();
    State().lastFrameTime = currentFrameTime;

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
            LoadScene(path);
            renderablesDirty = true;
        }
        catch (const std::exception& error)
        {
            State().lastSceneIoError = error.what();
            LOG_ERROR("Failed to load scene '{}': {}", path, error.what());
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
            LoadSelectedModel(path);
            renderablesDirty = true;
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to load model '{}': {}", path, error.what());
        }

        State().lastFrameTime = std::chrono::steady_clock::now();
    }

    State().renderablesDirty = false;
    return renderablesDirty;
}

void EditorRenderBackendBase::ApplyUiActions(const EditorUiFrameResult& uiFrame)
{
    State().requestedViewportExtent = uiFrame.viewportExtent;

    if (uiFrame.actions.selectedModelPath.has_value())
    {
        State().pendingModelPath = *uiFrame.actions.selectedModelPath;
    }
    if (uiFrame.actions.selectedSceneLoadPath.has_value())
    {
        State().pendingScenePath = *uiFrame.actions.selectedSceneLoadPath;
    }
    if (uiFrame.actions.selectedSceneSavePath.has_value())
    {
        try
        {
            State().editorScene.SaveSceneToFile(*uiFrame.actions.selectedSceneSavePath);
            State().editorScene.SetSceneFilePath(*uiFrame.actions.selectedSceneSavePath);
            State().lastSceneIoError.clear();
            LOG_INFO("Saved scene successfully: {}", *uiFrame.actions.selectedSceneSavePath);
        }
        catch (const std::exception& error)
        {
            State().lastSceneIoError = error.what();
            LOG_ERROR("Failed to save scene '{}': {}", *uiFrame.actions.selectedSceneSavePath, error.what());
        }
    }

    if (uiFrame.actions.requestedBackendType.has_value() &&
        *uiFrame.actions.requestedBackendType != m_backendType)
    {
        State().requestedBackendSwitch = *uiFrame.actions.requestedBackendType;
    }
}

void EditorRenderBackendBase::UpdateViewportMatrices(RenderExtent extent)
{
    const bool useVulkanClipSpace = m_backendType == RenderBackendType::Vulkan;
    State().viewportMatrices.view = State().camera.GetViewMatrix();
    State().viewportMatrices.projection = State().camera.GetProjectionMatrix(extent, false, useVulkanClipSpace);
    State().viewportMatrices.renderProjection = State().camera.GetProjectionMatrix(extent, useVulkanClipSpace, useVulkanClipSpace);
    State().viewportMatrices.model =
        State().editorScene.HasSelection() ? State().editorScene.GetModelMatrix(State().editorScene.GetSelectedEntity()) : glm::mat4(1.0f);
}

EditorUiFrameResult EditorRenderBackendBase::DrawEditorUi(ImTextureID viewportTextureId, RenderExtent viewportExtent)
{
    const std::string selectedModelPath =
        State().editorScene.HasSelection() ? State().editorScene.GetSelectedModel().sourcePath : std::string{};

    return State().editorUi.Draw(
        State().camera,
        State().viewportMatrices,
        State().editorScene,
        selectedModelPath,
        State().lastModelLoadError,
        State().lastSceneIoError,
        viewportTextureId,
        viewportExtent,
        m_backendType
    );
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

    if (!blockKeyboardInput)
    {
        if (input.IsKeyDown(SDL_SCANCODE_W))
        {
            camera.MoveForward(moveDistance);
        }
        if (input.IsKeyDown(SDL_SCANCODE_S))
        {
            camera.MoveForward(-moveDistance);
        }
        if (input.IsKeyDown(SDL_SCANCODE_A))
        {
            camera.MoveRight(-moveDistance);
        }
        if (input.IsKeyDown(SDL_SCANCODE_D))
        {
            camera.MoveRight(moveDistance);
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

void EditorRenderBackendBase::EnsureInitialized(std::optional<std::string> startupModelPath)
{
    if (State().initialized)
    {
        State().lastFrameTime = std::chrono::steady_clock::now();
        return;
    }

    InitializeEditorScene();
    State().editorScene.CreateTwoCubeTestScene();
    if (startupModelPath.has_value())
    {
        State().pendingModelPath = *startupModelPath;
    }

    RebuildSceneRenderables();
    State().initialized = true;
    State().renderablesDirty = true;
    State().lastFrameTime = std::chrono::steady_clock::now();
}

void EditorRenderBackendBase::InitializeEditorScene()
{
    State().editorScene.LoadConfig(MINIENGINE_ASSET_DIR "/editor/default_scene.yaml");
}

void EditorRenderBackendBase::LoadSelectedModel(const std::string& path, bool resetTransform)
{
    if (!State().editorScene.HasSelection())
    {
        throw std::runtime_error("No selected entity available to receive the model");
    }

    entt::entity selectedEntity = State().editorScene.GetSelectedEntity();
    ModelComponent previousModel = State().editorScene.GetModel(selectedEntity);
    State().editorScene.GetModel(selectedEntity).sourcePath = path;
    State().editorScene.GetModel(selectedEntity).displayName = std::filesystem::path(path).filename().string();

    try
    {
        RebuildSceneRenderables();
    }
    catch (...)
    {
        State().editorScene.GetModel(selectedEntity) = previousModel;
        throw;
    }

    const ModelComponent& model = State().editorScene.GetModel(selectedEntity);
    if (model.hasBounds)
    {
        State().camera.FrameBounds(model.minBounds, model.maxBounds);
    }

    State().lastModelLoadError.clear();
    if (resetTransform)
    {
        State().editorScene.ResetSelectedTransform();
    }
    LOG_INFO("Loaded model successfully into '{}': {}", State().editorScene.GetTag(selectedEntity).name, path);
}

void EditorRenderBackendBase::LoadScene(const std::string& path)
{
    const SerializedSceneData sceneData = EditorScene::LoadSceneDataFromFile(path);
    State().editorScene.ApplySceneData(sceneData);
    RebuildSceneRenderables();
    State().editorScene.SetSceneFilePath(path);
    State().lastSceneIoError.clear();
    LOG_INFO("Loaded scene successfully: {}", path);
}

void EditorRenderBackendBase::RebuildSceneRenderables()
{
    std::vector<CpuRenderSubmesh> newRenderSubmeshes;

    State().editorScene.ForEachEntity([&](
        entt::entity entity,
        const TagComponent& tag,
        const TransformComponent&,
        const ModelComponent& model
    )
    {
        if (model.sourcePath.empty())
        {
            CpuRenderSubmesh renderSubmesh{};
            renderSubmesh.entity = entity;
            renderSubmesh.mesh = CreateDefaultCubeMesh();
            renderSubmesh.material = BuildDefaultMaterialForTag(tag.name);
            renderSubmesh.hasTexCoords = true;
            renderSubmesh.name = tag.name;
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
            return;
        }

        const LoadedModelData modelData = ModelLoader::LoadModel(model.sourcePath);

        for (const ModelSubmeshData& submesh : modelData.submeshes)
        {
            CpuRenderSubmesh renderSubmesh{};
            renderSubmesh.entity = entity;
            renderSubmesh.mesh = submesh.mesh;
            renderSubmesh.hasTexCoords = submesh.hasTexCoords;

            const ModelMaterialData& material = modelData.materials[submesh.materialIndex];
            renderSubmesh.material.baseColorFactor[0] = material.baseColor[0];
            renderSubmesh.material.baseColorFactor[1] = material.baseColor[1];
            renderSubmesh.material.baseColorFactor[2] = material.baseColor[2];
            renderSubmesh.material.baseColorFactor[3] = material.baseColor[3];
            renderSubmesh.material.emissiveFactor[0] = material.emissiveColor[0] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[1] = material.emissiveColor[1] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[2] = material.emissiveColor[2] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[3] = material.emissiveIntensity;
            renderSubmesh.material.surfaceFactors[0] = material.metallicFactor;
            renderSubmesh.material.surfaceFactors[1] = material.roughnessFactor;
            renderSubmesh.material.surfaceFactors[2] = material.normalScale;
            renderSubmesh.material.surfaceFactors[3] = material.occlusionStrength;
            renderSubmesh.name = submesh.name;
            if (submesh.hasTexCoords)
            {
                renderSubmesh.textures.baseColor = material.baseColorTexturePath;
                renderSubmesh.textures.normal = material.normalTexturePath;
                renderSubmesh.textures.metallic = material.metallicTexturePath;
                renderSubmesh.textures.roughness = material.roughnessTexturePath;
                renderSubmesh.textures.occlusion = material.occlusionTexturePath;
                renderSubmesh.textures.emissive = material.emissiveTexturePath;
            }
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
        }

        State().editorScene.UpdateModelInfo(
            entity,
            model.displayName,
            model.sourcePath,
            static_cast<uint32_t>(modelData.submeshes.size()),
            modelData.minBounds,
            modelData.maxBounds,
            modelData.hasBounds
        );
    });

    State().renderSubmeshes = std::move(newRenderSubmeshes);
    State().renderablesDirty = true;
}
