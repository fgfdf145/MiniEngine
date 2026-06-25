#include "editor_backend_base.h"

#include <log/log.h>
#include <window/window.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

// =============================================================================
// [ASSET] Helper utilities — data conversion between scene/model types
// These helpers belong to the asset→render conversion boundary and will move
// to engine/asset/ once the SceneToRenderData pass is extracted.
// =============================================================================
namespace
{
// ---------------------------------------------------------------------------
// Model parse cache: path → parsed data.
// Written by background thread under lock; read by main thread under lock.
std::mutex s_modelCacheMutex;
std::unordered_map<std::string, std::shared_ptr<LoadedModelData>> s_modelCache;

bool IsModelCached(const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    return s_modelCache.count(path) > 0;
}

std::shared_ptr<LoadedModelData> GetCachedModel(const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(path);
    return it != s_modelCache.end() ? it->second : nullptr;
}

void CacheModel(const std::string& path, std::shared_ptr<LoadedModelData> data)
{
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    s_modelCache[path] = std::move(data);
}
// ---------------------------------------------------------------------------

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

ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData& material)
{
    return ModelImportedMaterialInfo{
        material.name,
        material.baseColorTexturePath,
        material.normalTexturePath,
        material.metallicTexturePath,
        material.roughnessTexturePath,
        material.occlusionTexturePath,
        material.emissiveTexturePath,
        material.pbr,
        material.blendGraph,
        material.shaderGraph
    };
}

ModelImportedSubmeshInfo BuildImportedSubmeshInfo(const ModelSubmeshData& submesh)
{
    return ModelImportedSubmeshInfo{
        submesh.name,
        static_cast<uint32_t>(submesh.mesh.vertices.size()),
        static_cast<uint32_t>(submesh.mesh.indices.size()),
        submesh.materialIndex,
        submesh.hasTexCoords,
        submesh.hasNormals,
        submesh.hasTangents
    };
}

std::filesystem::path NormalizePath(const std::filesystem::path& path);

std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, errorCode);
    return errorCode ? path.lexically_normal() : absolutePath.lexically_normal();
}

bool IsSceneAssetPath(const std::filesystem::path& path)
{
    const std::string extension = path.extension().string();
    if (extension != ".yaml" && extension != ".yml")
    {
        return false;
    }

    const std::string fileName = path.filename().string();
    if (fileName.ends_with(".material.yaml") || fileName.ends_with(".miniengine_asset.yaml"))
    {
        return false;
    }

    return true;
}

bool SceneDataReferencesModel(SerializedSceneData& sceneData, const std::filesystem::path& modelPath)
{
    bool referenced = false;
    for (SerializedEntityData& entity : sceneData.entities)
    {
        if (entity.modelSourcePath.empty())
        {
            continue;
        }

        if (NormalizePath(entity.modelSourcePath) != modelPath)
        {
            continue;
        }

        referenced = true;
        entity.modelDisplayName = modelPath.filename().string();
    }

    return referenced;
}

void ResetModelToBuiltin(ISceneWorld& scene, entt::entity entity)
{
    ModelComponent& model = scene.GetModel(entity);
    model.sourcePath.clear();
    model.displayName = scene.GetTag(entity).name;
    model.baseColorTextureOverridePath.clear();
    model.submeshCount = 1;
    model.minBounds = WorldUnits::kDefaultCubeMinBoundsMeters;
    model.maxBounds = WorldUnits::kDefaultCubeMaxBoundsMeters;
    model.hasBounds = true;
    model.importedMaterials.clear();
    model.importedSubmeshes.clear();
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
            if (EditorWorld().HasSelection())
            {
                LoadSelectedModel(path);
            }
            else
            {
                PlaceModelIntoScene(path, glm::vec3(0.0f));
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

    // --------------------------------------------------------------------------
    // Async model load completion
    // --------------------------------------------------------------------------
    AsyncModelLoad& load = State().asyncLoad;
    if (load.future.valid() &&
        load.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const auto IsValid = [&](entt::entity e) -> bool
        {
            if (e == entt::null)
            {
                return false;
            }
            const auto& order = EditorWorld().GetEntityOrder();
            return std::find(order.begin(), order.end(), e) != order.end();
        };

        try
        {
            load.future.get(); // re-throws if the background parse failed

            RebuildSceneRenderables();

            if (IsValid(load.trackedEntity))
            {
                const ModelComponent& model = EditorWorld().GetModel(load.trackedEntity);
                if (model.hasBounds)
                {
                    State().camera.FrameBounds(model.minBounds, model.maxBounds);
                }
                if (load.resetTransformOnComplete)
                {
                    EditorWorld().SetSelectedEntity(load.trackedEntity);
                    EditorWorld().ResetSelectedTransform();
                }
            }

            State().lastModelLoadError.clear();
            renderablesDirty = true;
            LOG_INFO("Async model load complete: {}", load.path);
        }
        catch (const std::exception& error)
        {
            if (IsValid(load.trackedEntity))
            {
                if (load.isReplacement)
                {
                    EditorWorld().GetModel(load.trackedEntity).sourcePath = load.previousSourcePath;
                    EditorWorld().GetModel(load.trackedEntity).displayName = load.previousDisplayName;
                }
                else
                {
                    EditorWorld().DestroyEntity(load.trackedEntity);
                    if (IsValid(load.previousSelection))
                    {
                        EditorWorld().SetSelectedEntity(load.previousSelection);
                    }
                }
            }

            State().lastModelLoadError = error.what();
            LOG_ERROR("Async model load failed for '{}': {}", load.path, error.what());
        }

        load.future = std::future<void>{}; // consume / reset
        State().lastFrameTime = std::chrono::steady_clock::now();
    }
    // --------------------------------------------------------------------------

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
            UpdateViewportModelPreview(*uiFrame.actions.hoveredViewportModel);
        }
        else
        {
            ClearViewportModelPreview();
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
            ImportModelIntoAssetDirectory(*uiFrame.actions.importedModelRequest);
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
            CreateSceneEntity();
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
            CreateSceneLightEntity(*uiFrame.actions.createLightEntity);
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
                DeleteSelectedLightEntity();
            }
            else
            {
                DeleteSelectedSceneEntity();
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
            CommitViewportModelPreview(*uiFrame.actions.droppedViewportModel);
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
            UpdateImportedMaterialDefinition(*uiFrame.actions.updatedImportedMaterial);
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
            UpdateImportedModelMaterialDefinitions(*uiFrame.actions.updatedImportedModelMaterials);
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
            ApplySelectedModelBaseColorTexture(*uiFrame.actions.selectedBaseColorTexturePath);
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
            ClearSelectedModelBaseColorTexture();
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
            EditorWorld().SaveSceneToFile(*uiFrame.actions.selectedSceneSavePath);
            EditorWorld().SetSceneFilePath(*uiFrame.actions.selectedSceneSavePath);
            State().lastSceneIoError.clear();
            LOG_INFO("Saved scene successfully: {}", *uiFrame.actions.selectedSceneSavePath);
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
            DeleteAssetPath(deletePath);
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
            PasteAssetPath(*uiFrame.actions.pastedAsset);
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
    if (State().asyncLoad.IsLoading())
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f));
        if (ImGui::Begin("##async_load_overlay", nullptr,
            ImGuiWindowFlags_NoDecoration  | ImGuiWindowFlags_NoInputs     |
            ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav         | ImGuiWindowFlags_AlwaysAutoResize))
        {
            const std::string filename =
                std::filesystem::path(State().asyncLoad.path).filename().string();

            const char* kSpinner = "|/-\\";
            const int spinFrame = static_cast<int>(ImGui::GetTime() * 10.0) & 3;
            ImGui::Text("%c  Loading: %s", kSpinner[spinFrame], filename.c_str());

            ImGui::Spacing();
            const float pulse =
                0.5f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
            ImGui::ProgressBar(pulse, ImVec2(-1.0f, 6.0f), "");
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

    RebuildSceneRenderables();
    State().initialized = true;
    State().renderablesDirty = true;
    State().lastFrameTime = std::chrono::steady_clock::now();
}

void EditorRenderBackendBase::InitializeEditorScene()
{
    EditorWorld().LoadConfig(MINIENGINE_ASSET_DIR "/editor/default_scene.yaml");
}

// =============================================================================
// [ASSET] Model import & file operations
// Will move to AssetManager / asset pipeline service.
// =============================================================================

std::string EditorRenderBackendBase::ImportModelIntoAssetDirectory(const EditorUiActions::ImportedModelRequest& request)
{
    const std::filesystem::path src = std::filesystem::path(request.sourcePath);
    if (!std::filesystem::exists(src))
    {
        throw std::runtime_error("Source file does not exist: " + request.sourcePath);
    }

    const std::filesystem::path dstDir = std::filesystem::path(request.destinationDirectory);
    const std::filesystem::path dst = dstDir / src.filename();

    std::error_code ec;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::skip_existing, ec);
    if (ec)
    {
        throw std::runtime_error("Failed to import '" + src.string() + "': " + ec.message());
    }

    // For .gltf (ASCII), also copy companion .bin and texture files from the same directory.
    const std::string ext = [&src]()
    {
        std::string e = src.extension().string();
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return e;
    }();

    if (ext == ".gltf")
    {
        // Recursively copy all texture / binary companion files from the source
        // directory tree into the same relative sub-path under dstDir.
        // This preserves the relative layout that the .gltf URIs rely on, so
        // moving the imported asset folder as a unit keeps all references valid.
        static constexpr std::array<std::string_view, 8> kAssetExts = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".dds", ".bin"
        };

        const std::filesystem::path srcDir = src.parent_path();
        for (const auto& item : std::filesystem::recursive_directory_iterator(srcDir, ec))
        {
            if (ec)
            {
                break;
            }
            if (!item.is_regular_file(ec) || ec)
            {
                continue;
            }

            std::string itemExt = item.path().extension().string();
            std::transform(itemExt.begin(), itemExt.end(), itemExt.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

            const bool isAsset = std::any_of(
                kAssetExts.begin(), kAssetExts.end(),
                [&itemExt](std::string_view e){ return itemExt == e; }
            );
            if (!isAsset)
            {
                continue;
            }

            // Compute the relative path from the source model directory and
            // mirror it under the destination directory.
            const std::filesystem::path relPath =
                item.path().lexically_relative(srcDir);
            const std::filesystem::path companionDst = dstDir / relPath;

            std::error_code mkdirEc;
            std::filesystem::create_directories(companionDst.parent_path(), mkdirEc);

            std::error_code copyEc;
            std::filesystem::copy_file(item.path(), companionDst,
                std::filesystem::copy_options::skip_existing, copyEc);
            if (copyEc)
            {
                LOG_WARN("Could not copy companion file '{}': {}",
                    item.path().string(), copyEc.message());
            }
            else
            {
                LOG_INFO("Copied companion: {} -> {}", item.path().string(), companionDst.string());
            }
        }
    }

    LOG_INFO("Imported model '{}' -> '{}'", src.string(), dst.string());
    return dst.string();
}

void EditorRenderBackendBase::DeleteAssetPath(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path), ec);
    if (ec)
    {
        throw std::runtime_error("Failed to delete '" + path + "': " + ec.message());
    }
    LOG_INFO("Deleted asset: {}", path);
}

void EditorRenderBackendBase::LoadSelectedModel(const std::string& path, bool resetTransform)
{
    if (State().asyncLoad.IsLoading())
    {
        throw std::runtime_error("Another model is currently loading. Please wait.");
    }

    if (!EditorWorld().HasSelection())
    {
        throw std::runtime_error("No selected entity available to receive the model");
    }

    const entt::entity selectedEntity = EditorWorld().GetSelectedEntity();
    const ModelComponent previousModel = EditorWorld().GetModel(selectedEntity);
    EditorWorld().GetModel(selectedEntity).sourcePath = path;
    EditorWorld().GetModel(selectedEntity).displayName = std::filesystem::path(path).filename().string();

    // Fast path: already cached.
    if (IsModelCached(path))
    {
        try
        {
            RebuildSceneRenderables();
            const ModelComponent& model = EditorWorld().GetModel(selectedEntity);
            if (model.hasBounds)
            {
                State().camera.FrameBounds(model.minBounds, model.maxBounds);
            }
        }
        catch (...)
        {
            EditorWorld().GetModel(selectedEntity) = previousModel;
            throw;
        }
        State().lastModelLoadError.clear();
        if (resetTransform)
        {
            EditorWorld().ResetSelectedTransform();
        }
        LOG_INFO("Loaded model (cached) into '{}': {}", EditorWorld().GetTag(selectedEntity).name, path);
        return;
    }

    // Async path: parse in background, rebuild when done.
    AsyncModelLoad& load = State().asyncLoad;
    load.path = path;
    load.isReplacement = true;
    load.worldPosition = glm::vec3(0.0f);
    load.trackedEntity = selectedEntity;
    load.previousSelection = entt::null;
    load.resetTransformOnComplete = resetTransform;
    load.previousSourcePath = previousModel.sourcePath;
    load.previousDisplayName = previousModel.displayName;

    load.future = std::async(std::launch::async, [p = path]()
    {
        auto data = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(p));
        CacheModel(p, std::move(data));
    });

    LOG_INFO("Started async load for: {}", path);
}

void EditorRenderBackendBase::PlaceModelIntoScene(const std::string& path, const glm::vec3& worldPosition)
{
    if (State().asyncLoad.IsLoading())
    {
        throw std::runtime_error("Another model is currently loading. Please wait.");
    }

    const std::filesystem::path modelPath = NormalizePath(path);
    if (!std::filesystem::exists(modelPath))
    {
        throw std::runtime_error("Dropped model asset does not exist: " + modelPath.string());
    }
    if (!ModelLoader::IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error("Dropped asset is not a supported glTF model: " + modelPath.string());
    }

    SerializedEntityData entityData{};
    entityData.tagName = modelPath.stem().string().empty() ? "Model" : modelPath.stem().string();
    entityData.modelDisplayName = modelPath.filename().string();
    entityData.modelSourcePath = modelPath.string();
    entityData.transform.translation = worldPosition;

    const entt::entity previousSelection =
        EditorWorld().HasSelection() ? EditorWorld().GetSelectedEntity() : entt::null;
    const entt::entity placedEntity = EditorWorld().CreateEntity(entityData);
    EditorWorld().SetSelectedEntity(placedEntity);

    // If already cached, rebuild synchronously (fast path).
    if (IsModelCached(modelPath.string()))
    {
        try
        {
            RebuildSceneRenderables();
            const ModelComponent& model = EditorWorld().GetModel(placedEntity);
            if (model.hasBounds)
            {
                State().camera.FrameBounds(model.minBounds, model.maxBounds);
            }
        }
        catch (...)
        {
            EditorWorld().DestroyEntity(placedEntity);
            EditorWorld().SetSelectedEntity(previousSelection);
            throw;
        }
        State().lastModelLoadError.clear();
        LOG_INFO("Placed model (cached) at ({:.3f}, {:.3f}, {:.3f}): {}",
            worldPosition.x, worldPosition.y, worldPosition.z, modelPath.string());
        return;
    }

    // Not cached: parse in background, rebuild when done.
    AsyncModelLoad& load = State().asyncLoad;
    load.path = modelPath.string();
    load.isReplacement = false;
    load.worldPosition = worldPosition;
    load.trackedEntity = placedEntity;
    load.previousSelection = previousSelection;
    load.resetTransformOnComplete = false;
    load.previousSourcePath.clear();
    load.previousDisplayName.clear();

    load.future = std::async(std::launch::async, [p = modelPath.string()]()
    {
        auto data = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(p));
        CacheModel(p, std::move(data));
    });

    LOG_INFO("Started async load for: {}", modelPath.string());
}

void EditorRenderBackendBase::UpdateViewportModelPreview(const EditorUiActions::ViewportModelPlacement& placement)
{
    const std::filesystem::path modelPath = NormalizePath(placement.modelPath);
    if (!std::filesystem::exists(modelPath))
    {
        throw std::runtime_error("Preview model asset does not exist: " + modelPath.string());
    }
    if (!ModelLoader::IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error("Preview asset is not a supported glTF model: " + modelPath.string());
    }

    ViewportDragPreviewState& preview = State().viewportDragPreview;
    const bool previewEntityStillExists =
        preview.active &&
        std::find(
            EditorWorld().GetEntityOrder().begin(),
            EditorWorld().GetEntityOrder().end(),
            preview.entity
        ) != EditorWorld().GetEntityOrder().end();

    if (!previewEntityStillExists || preview.modelPath != modelPath.string())
    {
        ClearViewportModelPreview();

        preview.previousSelection =
            EditorWorld().HasSelection() ? EditorWorld().GetSelectedEntity() : entt::null;
        preview.modelPath = modelPath.string();

        SerializedEntityData entityData{};
        entityData.tagName = modelPath.stem().string().empty() ? "Model" : modelPath.stem().string();
        entityData.modelDisplayName = modelPath.filename().string();
        entityData.modelSourcePath = modelPath.string();
        entityData.transform.translation = placement.worldPosition;

        const entt::entity previewEntity = EditorWorld().CreateEntity(entityData);

        try
        {
            RebuildSceneRenderables();
        }
        catch (...)
        {
            EditorWorld().DestroyEntity(previewEntity);
            EditorWorld().SetSelectedEntity(preview.previousSelection);
            preview = {};
            throw;
        }

        preview.active = true;
        preview.entity = previewEntity;
        if (preview.previousSelection != entt::null)
        {
            EditorWorld().SetSelectedEntity(preview.previousSelection);
        }
        else
        {
            EditorWorld().ClearSelection();
        }
        State().lastModelLoadError.clear();
        return;
    }

    EditorWorld().GetTransform(preview.entity).translation = placement.worldPosition;
}

void EditorRenderBackendBase::CommitViewportModelPreview(const EditorUiActions::ViewportModelPlacement& placement)
{
    ViewportDragPreviewState& preview = State().viewportDragPreview;
    const std::filesystem::path modelPath = NormalizePath(placement.modelPath);
    const bool previewEntityStillExists =
        preview.active &&
        preview.modelPath == modelPath.string() &&
        std::find(
            EditorWorld().GetEntityOrder().begin(),
            EditorWorld().GetEntityOrder().end(),
            preview.entity
        ) != EditorWorld().GetEntityOrder().end();

    if (!previewEntityStillExists)
    {
        PlaceModelIntoScene(modelPath.string(), placement.worldPosition);
        return;
    }

    EditorWorld().GetTransform(preview.entity).translation = placement.worldPosition;
    EditorWorld().SetSelectedEntity(preview.entity);
    preview = {};
    State().lastModelLoadError.clear();
    LOG_INFO(
        "Placed model asset into scene at ({:.3f}, {:.3f}, {:.3f}): {}",
        placement.worldPosition.x,
        placement.worldPosition.y,
        placement.worldPosition.z,
        modelPath.string()
    );
}

void EditorRenderBackendBase::ClearViewportModelPreview(bool restoreSelection)
{
    ViewportDragPreviewState preview = State().viewportDragPreview;
    if (!preview.active)
    {
        return;
    }

    State().viewportDragPreview = {};
    EditorWorld().DestroyEntity(preview.entity);
    if (restoreSelection)
    {
        if (preview.previousSelection != entt::null)
        {
            EditorWorld().SetSelectedEntity(preview.previousSelection);
        }
        else
        {
            EditorWorld().ClearSelection();
        }
    }

    RebuildSceneRenderables();
}

void EditorRenderBackendBase::UpdateImportedMaterialDefinition(const EditorUiActions::ImportedMaterialUpdate& update)
{
    static_cast<void>(update);
}

void EditorRenderBackendBase::UpdateImportedModelMaterialDefinitions(
    const EditorUiActions::ImportedModelMaterialsUpdate& update
)
{
    static_cast<void>(update);
}

void EditorRenderBackendBase::LoadScene(const std::string& path)
{
    const SerializedSceneData sceneData = LoadEditorSceneDataFromFile(path);
    EditorWorld().ApplySceneData(sceneData);
    RebuildSceneRenderables();
    EditorWorld().SetSceneFilePath(path);
    State().lastSceneIoError.clear();
    LOG_INFO("Loaded scene successfully: {}", path);
}

size_t EditorRenderBackendBase::RefreshReferencedSceneFiles(const std::filesystem::path& modelPath)
{
    size_t refreshedSceneCount = 0;
    std::error_code iteratorError;
    const std::filesystem::path workspaceRoot = NormalizePath(std::filesystem::current_path());

    for (std::filesystem::recursive_directory_iterator iterator(workspaceRoot, iteratorError), end;
         !iteratorError && iterator != end;
         iterator.increment(iteratorError))
    {
        if (!iterator->is_regular_file(iteratorError) || iteratorError)
        {
            continue;
        }

        const std::filesystem::path candidatePath = NormalizePath(iterator->path());
        if (!IsSceneAssetPath(candidatePath))
        {
            continue;
        }

        try
        {
            SerializedSceneData sceneData = LoadEditorSceneDataFromFile(candidatePath.string());
            if (!SceneDataReferencesModel(sceneData, modelPath))
            {
                continue;
            }

            SaveEditorSceneDataToFile(sceneData, candidatePath.string());
            ++refreshedSceneCount;
        }
        catch (...)
        {
            // Ignore non-scene YAML files and malformed sidecar data while scanning the workspace.
        }
    }

    return refreshedSceneCount;
}

// =============================================================================
// [EDITOR] Scene entity operations
// =============================================================================

void EditorRenderBackendBase::CreateSceneEntity()
{
    SerializedEntityData entityData{};
    entityData.tagName = "Entity " + std::to_string(EditorWorld().GetEntityOrder().size() + 1);
    entityData.modelDisplayName = entityData.tagName;
    const entt::entity entity = EditorWorld().CreateEntity(entityData);
    EditorWorld().SetSelectedEntity(entity);
    RebuildSceneRenderables();
    State().lastModelLoadError.clear();
    LOG_INFO("Created scene entity '{}'", entityData.tagName);
}

void EditorRenderBackendBase::DeleteSelectedSceneEntity()
{
    if (!EditorWorld().HasSelection())
    {
        throw std::runtime_error("No selected scene entity to delete");
    }

    const std::string tagName = EditorWorld().GetSelectedTag().name;
    EditorWorld().DestroyEntity(EditorWorld().GetSelectedEntity());
    RebuildSceneRenderables();
    State().lastModelLoadError.clear();
    LOG_INFO("Deleted scene entity '{}'", tagName);
}

// =============================================================================
// [ASSET] Asset file operations (paste / copy)
// =============================================================================

void EditorRenderBackendBase::PasteAssetPath(const EditorUiActions::AssetPasteRequest& request)
{
    const std::filesystem::path src = std::filesystem::path(request.sourcePath);
    const std::filesystem::path dst = std::filesystem::path(request.destinationDirectory) / src.filename();
    std::error_code eqEc;
    if (std::filesystem::equivalent(src, dst, eqEc) && !eqEc)
    {
        return;
    }
    std::error_code ec;
    std::filesystem::copy(src, dst,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing,
        ec);
    if (ec)
    {
        throw std::runtime_error(
            "Failed to copy '" + request.sourcePath + "' to '" + request.destinationDirectory + "': " + ec.message()
        );
    }
    LOG_INFO("Copied asset '{}' -> '{}'", request.sourcePath, dst.string());
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

void EditorRenderBackendBase::ApplySelectedModelBaseColorTexture(const std::string& path)
{
    if (!EditorWorld().HasSelection() ||
        EditorWorld().HasLightComponent(EditorWorld().GetSelectedEntity()))
    {
        throw std::runtime_error("No selected model entity available to receive the texture");
    }

    entt::entity selectedEntity = EditorWorld().GetSelectedEntity();
    ModelComponent previousModel = EditorWorld().GetModel(selectedEntity);
    ModelComponent& model = EditorWorld().GetModel(selectedEntity);
    if (model.sourcePath.empty())
    {
        throw std::runtime_error("The selected entity does not reference an imported model");
    }

    model.baseColorTextureOverridePath = path;

    try
    {
        RebuildSceneRenderables();
    }
    catch (...)
    {
        EditorWorld().GetModel(selectedEntity) = previousModel;
        throw;
    }

    State().lastModelLoadError.clear();
    LOG_INFO(
        "Applied selected texture override to '{}': {}",
        EditorWorld().GetTag(selectedEntity).name,
        path
    );
}

void EditorRenderBackendBase::ClearSelectedModelBaseColorTexture()
{
    if (!EditorWorld().HasSelection() ||
        EditorWorld().HasLightComponent(EditorWorld().GetSelectedEntity()))
    {
        throw std::runtime_error("No selected model entity available to clear the texture override");
    }

    entt::entity selectedEntity = EditorWorld().GetSelectedEntity();
    ModelComponent previousModel = EditorWorld().GetModel(selectedEntity);
    ModelComponent& model = EditorWorld().GetModel(selectedEntity);
    if (model.sourcePath.empty())
    {
        throw std::runtime_error("The selected entity does not reference an imported model");
    }

    model.baseColorTextureOverridePath.clear();

    try
    {
        RebuildSceneRenderables();
    }
    catch (...)
    {
        EditorWorld().GetModel(selectedEntity) = previousModel;
        throw;
    }

    State().lastModelLoadError.clear();
    LOG_INFO("Cleared selected texture override for '{}'", EditorWorld().GetTag(selectedEntity).name);
}

void EditorRenderBackendBase::CreateSceneLightEntity(const EditorUiActions::LightCreate& create)
{
    SerializedLightData lightData{};
    lightData.lightType = create.type;
    lightData.tagName = create.name;
    // Place the new light slightly in front of the camera
    lightData.transform.translation = State().camera.position +
        glm::vec3(State().viewportMatrices.view[0][2],
                  State().viewportMatrices.view[1][2],
                  State().viewportMatrices.view[2][2]) * -5.0f;
    const entt::entity entity = EditorWorld().CreateLightEntity(lightData);
    EditorWorld().SetSelectedEntity(entity);
    LOG_INFO("Created light entity '{}'", lightData.tagName);
}

void EditorRenderBackendBase::DeleteSelectedLightEntity()
{
    if (!EditorWorld().HasSelection())
    {
        throw std::runtime_error("No selected entity to delete");
    }

    const entt::entity selected = EditorWorld().GetSelectedEntity();
    if (!EditorWorld().HasLightComponent(selected))
    {
        throw std::runtime_error("Selected entity is not a light");
    }

    const std::string tagName = EditorWorld().GetTag(selected).name;
    EditorWorld().DestroyLightEntity(selected);
    LOG_INFO("Deleted light entity '{}'", tagName);
}

// =============================================================================
// [ASSET→RENDERER] Scene → CPU render submesh conversion
// Reads ModelLoader output, builds CpuRenderSubmesh list for Vulkan renderer.
// The model-loading half will move to engine/asset/; the renderer submission
// half stays here until a SceneToRenderData pass is introduced.
// =============================================================================

void EditorRenderBackendBase::RebuildSceneRenderables()
{
    std::vector<CpuRenderSubmesh> newRenderSubmeshes;

    EditorWorld().ForEachEntity([&](
        entt::entity entity,
        const TagComponent& tag,
        const TransformComponent&,
        const ModelComponent& model
    )
    {
        if (model.sourcePath.empty())
        {
            EditorWorld().UpdateModelInfo(
                entity,
                tag.name,
                std::string{},
                1,
                WorldUnits::kDefaultCubeMinBoundsMeters,
                WorldUnits::kDefaultCubeMaxBoundsMeters,
                true,
                {},
                {}
            );

            CpuRenderSubmesh renderSubmesh{};
            renderSubmesh.entity = entity;
            renderSubmesh.mesh = CreateDefaultCubeMesh();
            renderSubmesh.material = BuildDefaultMaterialForTag(tag.name);
            renderSubmesh.hasTexCoords = true;
            renderSubmesh.name = tag.name;
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
            return;
        }

        std::shared_ptr<LoadedModelData> modelDataPtr = GetCachedModel(model.sourcePath);
        if (!modelDataPtr)
        {
            modelDataPtr = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(model.sourcePath));
            CacheModel(model.sourcePath, modelDataPtr);
        }
        const LoadedModelData& modelData = *modelDataPtr;

        // Resolve texture paths relative to the model file's current directory.
        // This makes asset packages portable: moving model + textures together keeps
        // all relative references valid.
        const std::filesystem::path modelDir =
            std::filesystem::path(model.sourcePath).parent_path();
        const auto resolveTex = [&modelDir](const std::string& p) -> std::string
        {
            if (p.empty() || std::filesystem::path(p).is_absolute())
            {
                return p;
            }
            return (modelDir / p).lexically_normal().string();
        };

        std::vector<ModelImportedMaterialInfo> importedMaterials;
        importedMaterials.reserve(modelData.materials.size());
        for (const ModelMaterialData& rawMat : modelData.materials)
        {
            ModelMaterialData mat = rawMat;
            mat.baseColorTexturePath  = resolveTex(rawMat.baseColorTexturePath);
            mat.normalTexturePath     = resolveTex(rawMat.normalTexturePath);
            mat.metallicTexturePath   = resolveTex(rawMat.metallicTexturePath);
            mat.roughnessTexturePath  = resolveTex(rawMat.roughnessTexturePath);
            mat.occlusionTexturePath  = resolveTex(rawMat.occlusionTexturePath);
            mat.emissiveTexturePath   = resolveTex(rawMat.emissiveTexturePath);
            importedMaterials.push_back(BuildImportedMaterialInfo(mat));
        }

        std::vector<ModelImportedSubmeshInfo> importedSubmeshes;
        importedSubmeshes.reserve(modelData.submeshes.size());
        std::vector<bool> materialUsesUv(importedMaterials.size(), false);
        for (const ModelSubmeshData& submesh : modelData.submeshes)
        {
            importedSubmeshes.push_back(BuildImportedSubmeshInfo(submesh));
            if (submesh.hasTexCoords && submesh.materialIndex < materialUsesUv.size())
            {
                materialUsesUv[submesh.materialIndex] = true;
            }
        }
        if (!model.baseColorTextureOverridePath.empty())
        {
            for (size_t materialIndex = 0; materialIndex < importedMaterials.size(); ++materialIndex)
            {
                if (materialUsesUv[materialIndex])
                {
                    importedMaterials[materialIndex].baseColorTexturePath = model.baseColorTextureOverridePath;
                }
            }
        }

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
            renderSubmesh.material.baseColorFactor[3] = material.baseColor[3] * material.opacity;
            renderSubmesh.material.emissiveFactor[0] = material.emissiveColor[0] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[1] = material.emissiveColor[1] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[2] = material.emissiveColor[2] * material.emissiveIntensity;
            renderSubmesh.material.emissiveFactor[3] = material.alphaCutoff;
            renderSubmesh.material.surfaceFactors[0] = material.metallicFactor;
            renderSubmesh.material.surfaceFactors[1] = material.roughnessFactor;
            renderSubmesh.material.surfaceFactors[2] = material.normalScale;
            renderSubmesh.material.surfaceFactors[3] = material.occlusionStrength;
            renderSubmesh.material.nodeGraphFactors[0] = material.blendGraph.enabled ? 1.0f : 0.0f;
            renderSubmesh.material.nodeGraphFactors[1] = std::clamp(material.blendGraph.blendFactor, 0.0f, 1.0f);
            renderSubmesh.material.nodeGraphFactors[2] = 1.0f;
            renderSubmesh.material.nodeGraphFactors[3] = 0.0f;
            renderSubmesh.name = submesh.name;
            if (submesh.hasTexCoords)
            {
                renderSubmesh.textures.baseColor =
                    model.baseColorTextureOverridePath.empty()
                    ? resolveTex(material.baseColorTexturePath)
                    : model.baseColorTextureOverridePath;
                renderSubmesh.textures.normal    = resolveTex(material.normalTexturePath);
                renderSubmesh.textures.metallic  = resolveTex(material.metallicTexturePath);
                renderSubmesh.textures.roughness = resolveTex(material.roughnessTexturePath);
                renderSubmesh.textures.occlusion = resolveTex(material.occlusionTexturePath);
                renderSubmesh.textures.emissive  = resolveTex(material.emissiveTexturePath);
                renderSubmesh.textures.secondaryBaseColor =
                    material.blendGraph.secondaryBaseColorTexturePath.empty()
                    ? renderSubmesh.textures.baseColor
                    : material.blendGraph.secondaryBaseColorTexturePath;
                renderSubmesh.textures.secondaryNormal =
                    material.blendGraph.secondaryNormalTexturePath.empty()
                    ? renderSubmesh.textures.normal
                    : material.blendGraph.secondaryNormalTexturePath;
                renderSubmesh.textures.secondaryMetallic =
                    material.blendGraph.secondaryMetallicTexturePath.empty()
                    ? renderSubmesh.textures.metallic
                    : material.blendGraph.secondaryMetallicTexturePath;
                renderSubmesh.textures.secondaryRoughness =
                    material.blendGraph.secondaryRoughnessTexturePath.empty()
                    ? renderSubmesh.textures.roughness
                    : material.blendGraph.secondaryRoughnessTexturePath;
                renderSubmesh.textures.secondaryOcclusion =
                    material.blendGraph.secondaryOcclusionTexturePath.empty()
                    ? renderSubmesh.textures.occlusion
                    : material.blendGraph.secondaryOcclusionTexturePath;
                renderSubmesh.textures.secondaryEmissive =
                    material.blendGraph.secondaryEmissiveTexturePath.empty()
                    ? renderSubmesh.textures.emissive
                    : material.blendGraph.secondaryEmissiveTexturePath;
                renderSubmesh.textures.blendMask = material.blendGraph.blendMaskTexturePath;
            }
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
        }

        EditorWorld().UpdateModelInfo(
            entity,
            model.displayName,
            model.sourcePath,
            static_cast<uint32_t>(modelData.submeshes.size()),
            modelData.minBounds,
            modelData.maxBounds,
            modelData.hasBounds,
            importedMaterials,
            importedSubmeshes
        );
    });

    RenderWorld().SetRenderSubmeshes(std::move(newRenderSubmeshes));
    State().renderablesDirty = true;
}
