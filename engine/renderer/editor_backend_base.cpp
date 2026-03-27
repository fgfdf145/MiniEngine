#include "editor_backend_base.h"

#include <log/log.h>
#include <window/window.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
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

ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData& material)
{
    return ModelImportedMaterialInfo{
        material.name,
        material.baseColorTexturePath,
        material.normalTexturePath,
        material.metallicTexturePath,
        material.roughnessTexturePath,
        material.occlusionTexturePath,
        material.emissiveTexturePath
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

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsSupportedModelAssetPath(const std::filesystem::path& path)
{
    static constexpr std::array<const char*, 8> kExtensions = {
        ".obj", ".fbx", ".gltf", ".glb", ".dae", ".3ds", ".ply", ".stl"
    };

    const std::string extension = ToLowerCopy(path.extension().string());
    return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

std::filesystem::path BuildImportedAssetManifestPath(const std::filesystem::path& modelPath)
{
    return std::filesystem::path(modelPath.string() + ".miniengine_asset.yaml");
}

std::filesystem::path MakeUniquePath(const std::filesystem::path& desiredPath)
{
    if (!std::filesystem::exists(desiredPath))
    {
        return desiredPath;
    }

    const std::filesystem::path parentPath = desiredPath.parent_path();
    const std::string stem = desiredPath.stem().string();
    const std::string extension = desiredPath.extension().string();

    for (uint32_t suffix = 1; suffix < 10000; ++suffix)
    {
        const std::filesystem::path candidate =
            parentPath / (stem + "_" + std::to_string(suffix) + extension);
        if (!std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    throw std::runtime_error("Failed to create a unique asset path for: " + desiredPath.string());
}

std::filesystem::path MakeUniqueDirectoryPath(const std::filesystem::path& desiredPath)
{
    if (!std::filesystem::exists(desiredPath))
    {
        return desiredPath;
    }

    const std::filesystem::path parentPath = desiredPath.parent_path();
    const std::string baseName = desiredPath.filename().string();
    for (uint32_t suffix = 1; suffix < 10000; ++suffix)
    {
        const std::filesystem::path candidate =
            parentPath / (baseName + "_" + std::to_string(suffix));
        if (!std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    throw std::runtime_error("Failed to create a unique asset directory for: " + desiredPath.string());
}

std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, errorCode);
    return errorCode ? path.lexically_normal() : absolutePath.lexically_normal();
}

bool IsPathInsideDirectory(const std::filesystem::path& path, const std::filesystem::path& directory)
{
    const std::filesystem::path normalizedPath = NormalizePath(path);
    const std::filesystem::path normalizedDirectory = NormalizePath(directory);

    auto pathIterator = normalizedPath.begin();
    auto directoryIterator = normalizedDirectory.begin();
    for (; directoryIterator != normalizedDirectory.end(); ++directoryIterator, ++pathIterator)
    {
        if (pathIterator == normalizedPath.end() || *pathIterator != *directoryIterator)
        {
            return false;
        }
    }

    return true;
}

void ResetModelToBuiltin(EditorScene& scene, entt::entity entity)
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

    if (uiFrame.actions.importedModelSourcePath.has_value())
    {
        try
        {
            const std::string importedModelPath = ImportModelIntoAssetDirectory(*uiFrame.actions.importedModelSourcePath);
            if (State().editorScene.HasSelection())
            {
                State().pendingModelPath = importedModelPath;
            }
            State().lastModelLoadError.clear();
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to import model into assets '{}': {}", *uiFrame.actions.importedModelSourcePath, error.what());
        }
    }
    if (uiFrame.actions.selectedModelPath.has_value())
    {
        State().pendingModelPath = *uiFrame.actions.selectedModelPath;
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
    if (uiFrame.actions.deleteAssetPath.has_value())
    {
        try
        {
            DeleteAssetPath(*uiFrame.actions.deleteAssetPath);
        }
        catch (const std::exception& error)
        {
            State().lastModelLoadError = error.what();
            LOG_ERROR("Failed to delete asset '{}': {}", *uiFrame.actions.deleteAssetPath, error.what());
        }
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

std::string EditorRenderBackendBase::ImportModelIntoAssetDirectory(const std::string& sourcePath)
{
    const std::filesystem::path sourceModelPath = NormalizePath(sourcePath);
    if (!std::filesystem::exists(sourceModelPath))
    {
        throw std::runtime_error("Model source file does not exist: " + sourceModelPath.string());
    }
    if (!IsSupportedModelAssetPath(sourceModelPath))
    {
        throw std::runtime_error("Unsupported model format for asset import: " + sourceModelPath.string());
    }

    const std::filesystem::path assetModelsRoot = std::filesystem::path(MINIENGINE_ASSET_DIR) / "models";
    std::filesystem::create_directories(assetModelsRoot);

    const std::filesystem::path bundleDirectory =
        MakeUniqueDirectoryPath(assetModelsRoot / sourceModelPath.stem());
    std::filesystem::create_directories(bundleDirectory);

    const std::filesystem::path importedModelPath = bundleDirectory / sourceModelPath.filename();
    std::filesystem::copy_file(sourceModelPath, importedModelPath, std::filesystem::copy_options::overwrite_existing);

    const LoadedModelData sourceModelData = ModelLoader::LoadModel(sourceModelPath.string());
    const std::filesystem::path texturesDirectory = bundleDirectory / "textures";

    YAML::Emitter emitter;
    emitter << YAML::BeginMap;
    emitter << YAML::Key << "asset" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "version" << YAML::Value << 1;
    emitter << YAML::EndMap;
    emitter << YAML::Key << "materials" << YAML::Value << YAML::BeginSeq;

    auto copyTextureForManifest = [&](const std::string& texturePath, uint32_t materialIndex, const char* slotName) -> std::string
    {
        if (texturePath.empty())
        {
            return {};
        }

        const std::filesystem::path sourceTexturePath = NormalizePath(texturePath);
        if (!std::filesystem::exists(sourceTexturePath))
        {
            return texturePath;
        }

        std::filesystem::create_directories(texturesDirectory);
        const std::string destinationName =
            "material_" + std::to_string(materialIndex) + "_" + slotName + sourceTexturePath.extension().string();
        const std::filesystem::path destinationPath = MakeUniquePath(texturesDirectory / destinationName);
        std::filesystem::copy_file(sourceTexturePath, destinationPath, std::filesystem::copy_options::overwrite_existing);
        return destinationPath.string();
    };

    for (uint32_t materialIndex = 0; materialIndex < static_cast<uint32_t>(sourceModelData.materials.size()); ++materialIndex)
    {
        const ModelMaterialData& material = sourceModelData.materials[materialIndex];
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "name" << YAML::Value << material.name;
        emitter << YAML::Key << "base_color_texture_path" << YAML::Value
                << copyTextureForManifest(material.baseColorTexturePath, materialIndex, "base_color");
        emitter << YAML::Key << "normal_texture_path" << YAML::Value
                << copyTextureForManifest(material.normalTexturePath, materialIndex, "normal");
        emitter << YAML::Key << "metallic_texture_path" << YAML::Value
                << copyTextureForManifest(material.metallicTexturePath, materialIndex, "metallic");
        emitter << YAML::Key << "roughness_texture_path" << YAML::Value
                << copyTextureForManifest(material.roughnessTexturePath, materialIndex, "roughness");
        emitter << YAML::Key << "occlusion_texture_path" << YAML::Value
                << copyTextureForManifest(material.occlusionTexturePath, materialIndex, "occlusion");
        emitter << YAML::Key << "emissive_texture_path" << YAML::Value
                << copyTextureForManifest(material.emissiveTexturePath, materialIndex, "emissive");
        emitter << YAML::EndMap;
    }

    emitter << YAML::EndSeq;
    emitter << YAML::EndMap;

    std::ofstream output(BuildImportedAssetManifestPath(importedModelPath), std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        throw std::runtime_error("Failed to create imported asset manifest for: " + importedModelPath.string());
    }
    output << emitter.c_str();
    if (!output.good())
    {
        throw std::runtime_error("Failed to write imported asset manifest for: " + importedModelPath.string());
    }

    LOG_INFO("Imported model asset: {} -> {}", sourceModelPath.string(), importedModelPath.string());
    return importedModelPath.string();
}

void EditorRenderBackendBase::DeleteAssetPath(const std::string& path)
{
    const std::filesystem::path assetRoot = NormalizePath(std::filesystem::path(MINIENGINE_ASSET_DIR));
    const std::filesystem::path targetPath = NormalizePath(path);

    if (!std::filesystem::exists(targetPath))
    {
        throw std::runtime_error("Asset path does not exist: " + targetPath.string());
    }
    if (targetPath == assetRoot)
    {
        throw std::runtime_error("Deleting the root assets directory is not allowed");
    }
    if (!IsPathInsideDirectory(targetPath, assetRoot))
    {
        throw std::runtime_error("Deletion is only allowed inside the assets directory");
    }

    bool hadAffectedEntities = false;
    for (entt::entity entity : State().editorScene.GetEntityOrder())
    {
        ModelComponent& model = State().editorScene.GetModel(entity);
        if (model.sourcePath.empty())
        {
            continue;
        }

        const std::filesystem::path modelPath = NormalizePath(model.sourcePath);
        if (modelPath == targetPath || IsPathInsideDirectory(modelPath, targetPath))
        {
            ResetModelToBuiltin(State().editorScene, entity);
            hadAffectedEntities = true;
        }
    }

    if (std::filesystem::is_directory(targetPath))
    {
        std::filesystem::remove_all(targetPath);
    }
    else
    {
        std::filesystem::remove(targetPath);
        const std::filesystem::path manifestPath = BuildImportedAssetManifestPath(targetPath);
        if (std::filesystem::exists(manifestPath))
        {
            std::filesystem::remove(manifestPath);
        }
    }

    if (hadAffectedEntities)
    {
        RebuildSceneRenderables();
    }

    LOG_INFO("Deleted asset path: {}", targetPath.string());
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

void EditorRenderBackendBase::ApplySelectedModelBaseColorTexture(const std::string& path)
{
    if (!State().editorScene.HasSelection())
    {
        throw std::runtime_error("No selected entity available to receive the texture");
    }

    entt::entity selectedEntity = State().editorScene.GetSelectedEntity();
    ModelComponent previousModel = State().editorScene.GetModel(selectedEntity);
    ModelComponent& model = State().editorScene.GetModel(selectedEntity);
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
        State().editorScene.GetModel(selectedEntity) = previousModel;
        throw;
    }

    State().lastModelLoadError.clear();
    LOG_INFO(
        "Applied selected texture override to '{}': {}",
        State().editorScene.GetTag(selectedEntity).name,
        path
    );
}

void EditorRenderBackendBase::ClearSelectedModelBaseColorTexture()
{
    if (!State().editorScene.HasSelection())
    {
        throw std::runtime_error("No selected entity available to clear the texture override");
    }

    entt::entity selectedEntity = State().editorScene.GetSelectedEntity();
    ModelComponent previousModel = State().editorScene.GetModel(selectedEntity);
    ModelComponent& model = State().editorScene.GetModel(selectedEntity);
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
        State().editorScene.GetModel(selectedEntity) = previousModel;
        throw;
    }

    State().lastModelLoadError.clear();
    LOG_INFO("Cleared selected texture override for '{}'", State().editorScene.GetTag(selectedEntity).name);
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
            State().editorScene.UpdateModelInfo(
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

        const LoadedModelData modelData = ModelLoader::LoadModel(model.sourcePath);
        std::vector<ModelImportedMaterialInfo> importedMaterials;
        importedMaterials.reserve(modelData.materials.size());
        for (const ModelMaterialData& material : modelData.materials)
        {
            importedMaterials.push_back(BuildImportedMaterialInfo(material));
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
                renderSubmesh.textures.baseColor =
                    model.baseColorTextureOverridePath.empty()
                    ? material.baseColorTexturePath
                    : model.baseColorTextureOverridePath;
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
            modelData.hasBounds,
            importedMaterials,
            importedSubmeshes
        );
    });

    State().renderSubmeshes = std::move(newRenderSubmeshes);
    State().renderablesDirty = true;
}
