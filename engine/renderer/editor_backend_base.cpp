#include "editor_backend_base.h"

#include <log/log.h>
#include <window/window.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
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
        material.emissiveTexturePath,
        material.blendGraph
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

void EmitMaterialBlendGraph(YAML::Emitter& emitter, const MaterialTextureBlendGraph& blendGraph)
{
    const float clampedBlendFactor = std::clamp(blendGraph.blendFactor, 0.0f, 1.0f);
    emitter << YAML::Key << "texture_graph" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "enabled" << YAML::Value << blendGraph.enabled;
    emitter << YAML::Key << "blend_factor" << YAML::Value << clampedBlendFactor;
    emitter << YAML::Key << "blend_mask_texture_path" << YAML::Value << blendGraph.blendMaskTexturePath;
    emitter << YAML::Key << "secondary_base_color_texture_path" << YAML::Value << blendGraph.secondaryBaseColorTexturePath;
    emitter << YAML::Key << "secondary_normal_texture_path" << YAML::Value << blendGraph.secondaryNormalTexturePath;
    emitter << YAML::Key << "secondary_metallic_texture_path" << YAML::Value << blendGraph.secondaryMetallicTexturePath;
    emitter << YAML::Key << "secondary_roughness_texture_path" << YAML::Value << blendGraph.secondaryRoughnessTexturePath;
    emitter << YAML::Key << "secondary_occlusion_texture_path" << YAML::Value << blendGraph.secondaryOcclusionTexturePath;
    emitter << YAML::Key << "secondary_emissive_texture_path" << YAML::Value << blendGraph.secondaryEmissiveTexturePath;
    emitter << YAML::EndMap;
}

std::filesystem::path NormalizePath(const std::filesystem::path& path);

std::filesystem::path BuildImportedAssetManifestPath(const std::filesystem::path& modelPath)
{
    return std::filesystem::path(modelPath.string() + ".miniengine_asset.yaml");
}

std::filesystem::path BuildImportedMaterialAssetPath(const std::filesystem::path& modelPath, uint32_t materialIndex)
{
    std::ostringstream fileName;
    fileName << "material_" << std::setfill('0') << std::setw(2) << materialIndex << ".material.yaml";
    return modelPath.parent_path() / "materials" / fileName.str();
}

void EmitImportedMaterialFields(YAML::Emitter& emitter, const ModelImportedMaterialInfo& material)
{
    emitter << YAML::Key << "name" << YAML::Value << material.name;
    emitter << YAML::Key << "base_color_texture_path" << YAML::Value << material.baseColorTexturePath;
    emitter << YAML::Key << "normal_texture_path" << YAML::Value << material.normalTexturePath;
    emitter << YAML::Key << "metallic_texture_path" << YAML::Value << material.metallicTexturePath;
    emitter << YAML::Key << "roughness_texture_path" << YAML::Value << material.roughnessTexturePath;
    emitter << YAML::Key << "occlusion_texture_path" << YAML::Value << material.occlusionTexturePath;
    emitter << YAML::Key << "emissive_texture_path" << YAML::Value << material.emissiveTexturePath;
    EmitMaterialBlendGraph(emitter, material.blendGraph);
}

void WriteImportedMaterialAssetFile(
    const std::filesystem::path& materialAssetPath,
    const std::filesystem::path& modelPath,
    uint32_t materialIndex,
    const ModelImportedMaterialInfo& material
)
{
    std::filesystem::create_directories(materialAssetPath.parent_path());

    YAML::Emitter emitter;
    emitter << YAML::BeginMap;
    emitter << YAML::Key << "asset" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "version" << YAML::Value << 1;
    emitter << YAML::Key << "type" << YAML::Value << "material";
    emitter << YAML::Key << "model_path" << YAML::Value << modelPath.string();
    emitter << YAML::Key << "material_index" << YAML::Value << materialIndex;
    emitter << YAML::EndMap;
    emitter << YAML::Key << "material" << YAML::Value << YAML::BeginMap;
    EmitImportedMaterialFields(emitter, material);
    emitter << YAML::EndMap;
    emitter << YAML::EndMap;

    std::ofstream output(materialAssetPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        throw std::runtime_error("Failed to create imported material asset: " + materialAssetPath.string());
    }
    output << emitter.c_str();
    if (!output.good())
    {
        throw std::runtime_error("Failed to write imported material asset: " + materialAssetPath.string());
    }
}

YAML::Node LoadYamlMapOrEmpty(const std::filesystem::path& path)
{
    YAML::Node root =
        std::filesystem::exists(path)
        ? YAML::LoadFile(path.string())
        : YAML::Node(YAML::NodeType::Map);
    if (!root || !root.IsMap())
    {
        root = YAML::Node(YAML::NodeType::Map);
    }

    return root;
}

void EnsureImportedManifestVersionNode(YAML::Node& root)
{
    YAML::Node assetNode = root["asset"];
    if (!assetNode || !assetNode.IsMap())
    {
        assetNode = YAML::Node(YAML::NodeType::Map);
        root["asset"] = assetNode;
    }

    assetNode["version"] = assetNode["version"] ? assetNode["version"].as<int>(1) : 1;
}

void ValidateImportedMaterialTargetPath(const std::filesystem::path& modelPath)
{
    if (!std::filesystem::exists(modelPath))
    {
        throw std::runtime_error("Material target model does not exist: " + modelPath.string());
    }
    if (!ModelLoader::IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error("Material target is not a supported FBX model: " + modelPath.string());
    }
}

YAML::Node BuildMaterialBlendGraphNode(const MaterialTextureBlendGraph& blendGraph)
{
    YAML::Node graphNode(YAML::NodeType::Map);
    graphNode["enabled"] = blendGraph.enabled;
    graphNode["blend_factor"] = std::clamp(blendGraph.blendFactor, 0.0f, 1.0f);
    graphNode["blend_mask_texture_path"] = blendGraph.blendMaskTexturePath;
    graphNode["secondary_base_color_texture_path"] = blendGraph.secondaryBaseColorTexturePath;
    graphNode["secondary_normal_texture_path"] = blendGraph.secondaryNormalTexturePath;
    graphNode["secondary_metallic_texture_path"] = blendGraph.secondaryMetallicTexturePath;
    graphNode["secondary_roughness_texture_path"] = blendGraph.secondaryRoughnessTexturePath;
    graphNode["secondary_occlusion_texture_path"] = blendGraph.secondaryOcclusionTexturePath;
    graphNode["secondary_emissive_texture_path"] = blendGraph.secondaryEmissiveTexturePath;
    return graphNode;
}

YAML::Node BuildImportedMaterialManifestNode(
    const ModelImportedMaterialInfo& material,
    const std::filesystem::path& materialAssetPath
)
{
    YAML::Node materialNode(YAML::NodeType::Map);
    materialNode["name"] = material.name;
    materialNode["base_color_texture_path"] = material.baseColorTexturePath;
    materialNode["normal_texture_path"] = material.normalTexturePath;
    materialNode["metallic_texture_path"] = material.metallicTexturePath;
    materialNode["roughness_texture_path"] = material.roughnessTexturePath;
    materialNode["occlusion_texture_path"] = material.occlusionTexturePath;
    materialNode["emissive_texture_path"] = material.emissiveTexturePath;
    materialNode["texture_graph"] = BuildMaterialBlendGraphNode(material.blendGraph);
    materialNode["material_asset_path"] = materialAssetPath.string();
    return materialNode;
}

YAML::Node GetMaterialManifestNode(const YAML::Node& materialsNode, size_t materialIndex)
{
    if (!materialsNode || !materialsNode.IsSequence() || materialIndex >= materialsNode.size())
    {
        return {};
    }

    return materialsNode[materialIndex];
}

std::filesystem::path ResolveMaterialAssetPath(
    const std::filesystem::path& modelPath,
    uint32_t materialIndex,
    const YAML::Node& materialNode
)
{
    const std::string existingMaterialAssetPath =
        materialNode && materialNode.IsMap() && materialNode["material_asset_path"]
        ? materialNode["material_asset_path"].as<std::string>()
        : std::string{};

    return existingMaterialAssetPath.empty()
        ? BuildImportedMaterialAssetPath(modelPath, materialIndex)
        : NormalizePath(existingMaterialAssetPath);
}

void WriteYamlDocument(const YAML::Node& root, const std::filesystem::path& path, const char* description)
{
    YAML::Emitter emitter;
    emitter << root;
    if (!emitter.good())
    {
        throw std::runtime_error(std::string("Failed to serialize ") + description + ": " + path.string());
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        throw std::runtime_error(std::string("Failed to open ") + description + " for writing: " + path.string());
    }
    output << emitter.c_str();
    if (!output.good())
    {
        throw std::runtime_error(std::string("Failed to write ") + description + ": " + path.string());
    }
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

struct ImportedModelFingerprint
{
    uintmax_t fileSize = 0;
    std::string contentHash;
};

std::string ComputeFileContentHash(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return {};
    }

    uint64_t hash = 1469598103934665603ull;
    std::array<char, 8192> buffer{};
    while (input.good())
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        for (std::streamsize index = 0; index < bytesRead; ++index)
        {
            hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(index)]);
            hash *= 1099511628211ull;
        }
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

ImportedModelFingerprint ComputeImportedModelFingerprint(const std::filesystem::path& path)
{
    ImportedModelFingerprint fingerprint{};
    std::error_code errorCode;
    fingerprint.fileSize = std::filesystem::file_size(path, errorCode);
    if (errorCode)
    {
        fingerprint.fileSize = 0;
    }

    fingerprint.contentHash = ComputeFileContentHash(path);
    return fingerprint;
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

    EditorUiFrameResult result = State().editorUi.Draw(
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

    if (result.engineSettingsChanged || State().engineSettingsNeedsBootstrapSave)
    {
        State().editorUi.WriteEngineSettings(State().engineSettings);
        SaveEngineSettings();
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
    if (!ModelLoader::IsSupportedModelPath(sourceModelPath))
    {
        throw std::runtime_error("MiniEngine only imports FBX models (*.fbx): " + sourceModelPath.string());
    }

    const std::filesystem::path assetModelsRoot = std::filesystem::path(MINIENGINE_ASSET_DIR) / "models";
    std::filesystem::create_directories(assetModelsRoot);

    const std::filesystem::path bundleDirectory =
        MakeUniqueDirectoryPath(assetModelsRoot / sourceModelPath.stem());
    std::filesystem::create_directories(bundleDirectory);

    const std::filesystem::path importedModelPath = bundleDirectory / sourceModelPath.filename();
    std::filesystem::copy_file(sourceModelPath, importedModelPath, std::filesystem::copy_options::overwrite_existing);
    const ImportedModelFingerprint sourceFingerprint = ComputeImportedModelFingerprint(sourceModelPath);

    const LoadedModelData sourceModelData = ModelLoader::LoadModel(sourceModelPath.string());
    const std::filesystem::path texturesDirectory = bundleDirectory / "textures";
    const std::filesystem::path materialsDirectory = bundleDirectory / "materials";

    YAML::Emitter emitter;
    emitter << YAML::BeginMap;
    emitter << YAML::Key << "asset" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "version" << YAML::Value << 1;
    emitter << YAML::Key << "source" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "original_path" << YAML::Value << sourceModelPath.string();
    emitter << YAML::Key << "file_size" << YAML::Value << sourceFingerprint.fileSize;
    emitter << YAML::Key << "content_hash" << YAML::Value << sourceFingerprint.contentHash;
    emitter << YAML::EndMap;
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
        ModelImportedMaterialInfo importedMaterial = BuildImportedMaterialInfo(material);
        importedMaterial.baseColorTexturePath = copyTextureForManifest(material.baseColorTexturePath, materialIndex, "base_color");
        importedMaterial.normalTexturePath = copyTextureForManifest(material.normalTexturePath, materialIndex, "normal");
        importedMaterial.metallicTexturePath = copyTextureForManifest(material.metallicTexturePath, materialIndex, "metallic");
        importedMaterial.roughnessTexturePath = copyTextureForManifest(material.roughnessTexturePath, materialIndex, "roughness");
        importedMaterial.occlusionTexturePath = copyTextureForManifest(material.occlusionTexturePath, materialIndex, "occlusion");
        importedMaterial.emissiveTexturePath = copyTextureForManifest(material.emissiveTexturePath, materialIndex, "emissive");
        importedMaterial.blendGraph.blendMaskTexturePath =
            copyTextureForManifest(material.blendGraph.blendMaskTexturePath, materialIndex, "blend_mask");
        importedMaterial.blendGraph.secondaryBaseColorTexturePath =
            copyTextureForManifest(material.blendGraph.secondaryBaseColorTexturePath, materialIndex, "secondary_base_color");
        importedMaterial.blendGraph.secondaryNormalTexturePath =
            copyTextureForManifest(material.blendGraph.secondaryNormalTexturePath, materialIndex, "secondary_normal");
        importedMaterial.blendGraph.secondaryMetallicTexturePath =
            copyTextureForManifest(material.blendGraph.secondaryMetallicTexturePath, materialIndex, "secondary_metallic");
        importedMaterial.blendGraph.secondaryRoughnessTexturePath =
            copyTextureForManifest(material.blendGraph.secondaryRoughnessTexturePath, materialIndex, "secondary_roughness");
        importedMaterial.blendGraph.secondaryOcclusionTexturePath =
            copyTextureForManifest(material.blendGraph.secondaryOcclusionTexturePath, materialIndex, "secondary_occlusion");
        importedMaterial.blendGraph.secondaryEmissiveTexturePath =
            copyTextureForManifest(material.blendGraph.secondaryEmissiveTexturePath, materialIndex, "secondary_emissive");

        const std::filesystem::path materialAssetPath =
            MakeUniquePath(materialsDirectory / BuildImportedMaterialAssetPath(importedModelPath, materialIndex).filename());
        WriteImportedMaterialAssetFile(materialAssetPath, importedModelPath, materialIndex, importedMaterial);

        emitter << YAML::BeginMap;
        EmitImportedMaterialFields(emitter, importedMaterial);
        emitter << YAML::Key << "material_asset_path" << YAML::Value << materialAssetPath.string();
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

    if (State().editorScene.HasEntities())
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

void EditorRenderBackendBase::PlaceModelIntoScene(const std::string& path, const glm::vec3& worldPosition)
{
    const std::filesystem::path modelPath = NormalizePath(path);
    if (!std::filesystem::exists(modelPath))
    {
        throw std::runtime_error("Dropped model asset does not exist: " + modelPath.string());
    }
    if (!ModelLoader::IsSupportedModelPath(modelPath))
    {
        throw std::runtime_error("Dropped asset is not a supported FBX model: " + modelPath.string());
    }

    SerializedEntityData entityData{};
    entityData.tagName = modelPath.stem().string().empty() ? "Model" : modelPath.stem().string();
    entityData.modelDisplayName = modelPath.filename().string();
    entityData.modelSourcePath = modelPath.string();
    entityData.transform.translation = worldPosition;

    const entt::entity previousSelection =
        State().editorScene.HasSelection() ? State().editorScene.GetSelectedEntity() : entt::null;
    const entt::entity placedEntity = State().editorScene.CreateEntity(entityData);
    State().editorScene.SetSelectedEntity(placedEntity);

    try
    {
        RebuildSceneRenderables();
    }
    catch (...)
    {
        State().editorScene.DestroyEntity(placedEntity);
        State().editorScene.SetSelectedEntity(previousSelection);
        throw;
    }

    State().lastModelLoadError.clear();
    LOG_INFO(
        "Placed model asset into scene at ({:.3f}, {:.3f}, {:.3f}): {}",
        worldPosition.x,
        worldPosition.y,
        worldPosition.z,
        modelPath.string()
    );
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
        throw std::runtime_error("Preview asset is not a supported FBX model: " + modelPath.string());
    }

    ViewportDragPreviewState& preview = State().viewportDragPreview;
    const bool previewEntityStillExists =
        preview.active &&
        std::find(
            State().editorScene.GetEntityOrder().begin(),
            State().editorScene.GetEntityOrder().end(),
            preview.entity
        ) != State().editorScene.GetEntityOrder().end();

    if (!previewEntityStillExists || preview.modelPath != modelPath.string())
    {
        ClearViewportModelPreview();

        preview.previousSelection =
            State().editorScene.HasSelection() ? State().editorScene.GetSelectedEntity() : entt::null;
        preview.modelPath = modelPath.string();

        SerializedEntityData entityData{};
        entityData.tagName = modelPath.stem().string().empty() ? "Model" : modelPath.stem().string();
        entityData.modelDisplayName = modelPath.filename().string();
        entityData.modelSourcePath = modelPath.string();
        entityData.transform.translation = placement.worldPosition;

        const entt::entity previewEntity = State().editorScene.CreateEntity(entityData);

        try
        {
            RebuildSceneRenderables();
        }
        catch (...)
        {
            State().editorScene.DestroyEntity(previewEntity);
            State().editorScene.SetSelectedEntity(preview.previousSelection);
            preview = {};
            throw;
        }

        preview.active = true;
        preview.entity = previewEntity;
        if (preview.previousSelection != entt::null)
        {
            State().editorScene.SetSelectedEntity(preview.previousSelection);
        }
        else
        {
            State().editorScene.ClearSelection();
        }
        State().lastModelLoadError.clear();
        return;
    }

    State().editorScene.GetTransform(preview.entity).translation = placement.worldPosition;
}

void EditorRenderBackendBase::CommitViewportModelPreview(const EditorUiActions::ViewportModelPlacement& placement)
{
    ViewportDragPreviewState& preview = State().viewportDragPreview;
    const std::filesystem::path modelPath = NormalizePath(placement.modelPath);
    const bool previewEntityStillExists =
        preview.active &&
        preview.modelPath == modelPath.string() &&
        std::find(
            State().editorScene.GetEntityOrder().begin(),
            State().editorScene.GetEntityOrder().end(),
            preview.entity
        ) != State().editorScene.GetEntityOrder().end();

    if (!previewEntityStillExists)
    {
        PlaceModelIntoScene(modelPath.string(), placement.worldPosition);
        return;
    }

    State().editorScene.GetTransform(preview.entity).translation = placement.worldPosition;
    State().editorScene.SetSelectedEntity(preview.entity);
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
    State().editorScene.DestroyEntity(preview.entity);
    if (restoreSelection)
    {
        if (preview.previousSelection != entt::null)
        {
            State().editorScene.SetSelectedEntity(preview.previousSelection);
        }
        else
        {
            State().editorScene.ClearSelection();
        }
    }

    RebuildSceneRenderables();
}

void EditorRenderBackendBase::UpdateImportedMaterialDefinition(const EditorUiActions::ImportedMaterialUpdate& update)
{
    const std::filesystem::path modelPath = NormalizePath(update.modelPath);
    ValidateImportedMaterialTargetPath(modelPath);

    const std::filesystem::path manifestPath = BuildImportedAssetManifestPath(modelPath);
    YAML::Node root = LoadYamlMapOrEmpty(manifestPath);
    EnsureImportedManifestVersionNode(root);

    YAML::Node materialsNode = root["materials"];
    if (!materialsNode || !materialsNode.IsSequence())
    {
        materialsNode = YAML::Node(YAML::NodeType::Sequence);
    }
    while (materialsNode.size() <= update.materialIndex)
    {
        materialsNode.push_back(YAML::Node(YAML::NodeType::Map));
    }

    ModelImportedMaterialInfo material = update.material;
    material.blendGraph.blendFactor = std::clamp(material.blendGraph.blendFactor, 0.0f, 1.0f);

    const std::filesystem::path materialAssetPath =
        ResolveMaterialAssetPath(modelPath, update.materialIndex, materialsNode[update.materialIndex]);
    materialsNode[update.materialIndex] = BuildImportedMaterialManifestNode(material, materialAssetPath);

    root["materials"] = materialsNode;
    WriteYamlDocument(root, manifestPath, "imported material manifest");
    WriteImportedMaterialAssetFile(materialAssetPath, modelPath, update.materialIndex, material);

    RebuildSceneRenderables();
    State().lastModelLoadError.clear();
    LOG_INFO(
        "Updated programmable PBR graph for '{}' material index {}",
        modelPath.string(),
        update.materialIndex
    );
}

void EditorRenderBackendBase::UpdateImportedModelMaterialDefinitions(
    const EditorUiActions::ImportedModelMaterialsUpdate& update
)
{
    const std::filesystem::path modelPath = NormalizePath(update.modelPath);
    ValidateImportedMaterialTargetPath(modelPath);

    const std::filesystem::path manifestPath = BuildImportedAssetManifestPath(modelPath);
    YAML::Node root = LoadYamlMapOrEmpty(manifestPath);
    EnsureImportedManifestVersionNode(root);

    const YAML::Node existingMaterialsNode = root["materials"];
    YAML::Node materialsNode(YAML::NodeType::Sequence);

    for (size_t materialIndex = 0; materialIndex < update.materials.size(); ++materialIndex)
    {
        ModelImportedMaterialInfo material = update.materials[materialIndex];
        material.blendGraph.blendFactor = std::clamp(material.blendGraph.blendFactor, 0.0f, 1.0f);

        const std::filesystem::path materialAssetPath =
            ResolveMaterialAssetPath(
                modelPath,
                static_cast<uint32_t>(materialIndex),
                GetMaterialManifestNode(existingMaterialsNode, materialIndex)
            );
        materialsNode.push_back(BuildImportedMaterialManifestNode(material, materialAssetPath));

        WriteImportedMaterialAssetFile(
            materialAssetPath,
            modelPath,
            static_cast<uint32_t>(materialIndex),
            material
        );
    }

    root["materials"] = materialsNode;
    WriteYamlDocument(root, manifestPath, "imported material manifest");

    RebuildSceneRenderables();
    State().lastModelLoadError.clear();
    LOG_INFO(
        "Updated imported model materials for '{}' with {} slot(s)",
        modelPath.string(),
        update.materials.size()
    );
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

void EditorRenderBackendBase::PasteAssetPath(const EditorUiActions::AssetPasteRequest& request)
{
    const std::filesystem::path assetRoot = NormalizePath(std::filesystem::path(MINIENGINE_ASSET_DIR));
    const std::filesystem::path sourcePath = NormalizePath(request.sourcePath);
    const std::filesystem::path destinationDirectory = NormalizePath(request.destinationDirectory);

    if (!std::filesystem::exists(sourcePath))
    {
        throw std::runtime_error("Copied asset no longer exists: " + sourcePath.string());
    }
    if (!std::filesystem::exists(destinationDirectory) || !std::filesystem::is_directory(destinationDirectory))
    {
        throw std::runtime_error("Paste destination is not a valid directory: " + destinationDirectory.string());
    }
    if (!IsPathInsideDirectory(sourcePath, assetRoot) || !IsPathInsideDirectory(destinationDirectory, assetRoot))
    {
        throw std::runtime_error("Copy and paste are only allowed inside the assets directory");
    }
    if (std::filesystem::is_directory(sourcePath) && IsPathInsideDirectory(destinationDirectory, sourcePath))
    {
        throw std::runtime_error("Cannot paste a folder into itself or one of its descendants");
    }

    const std::filesystem::path destinationPath =
        std::filesystem::is_directory(sourcePath)
        ? MakeUniqueDirectoryPath(destinationDirectory / sourcePath.filename())
        : MakeUniquePath(destinationDirectory / sourcePath.filename());

    std::filesystem::copy(
        sourcePath,
        destinationPath,
        std::filesystem::copy_options::recursive
    );

    State().lastModelLoadError.clear();
    LOG_INFO("Copied asset '{}' to '{}'", sourcePath.string(), destinationPath.string());
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
            renderSubmesh.material.nodeGraphFactors[0] = material.blendGraph.enabled ? 1.0f : 0.0f;
            renderSubmesh.material.nodeGraphFactors[1] = std::clamp(material.blendGraph.blendFactor, 0.0f, 1.0f);
            renderSubmesh.material.nodeGraphFactors[2] = 1.0f;
            renderSubmesh.material.nodeGraphFactors[3] = 0.0f;
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
