#include "renderer.h"

#include "../imgui/imgui_impl_vulkan.h"

#include <imgui.h>
#include <log/log.h>
#include <window/window.h>

#include <cstdint>
#include <filesystem>
#include <unordered_map>

namespace
{
const char* kExampleTexturePath = MINIENGINE_ASSET_DIR "/textures/checkerboard.ppm";

TextureData CreateSolidTexture(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
{
    TextureData texture{};
    texture.width = 1;
    texture.height = 1;
    texture.channelCount = 4;
    texture.pixels = { red, green, blue, alpha };
    return texture;
}

TextureData CreateFlatNormalTexture()
{
    return CreateSolidTexture(128, 128, 255, 255);
}

std::string BuildTextureCacheKey(const std::string& path, VulkanTextureFormat textureFormat)
{
    return path + (textureFormat == VulkanTextureFormat::SrgbColor ? "|srgb" : "|linear");
}

std::vector<MaterialTextureBinding> BuildMaterialTextureBindings(
    const std::vector<std::unique_ptr<VulkanTexture>>& textures,
    const std::vector<MaterialTextureSlots>& materialTextureSlots
)
{
    std::vector<MaterialTextureBinding> bindings;
    bindings.reserve(materialTextureSlots.size());

    for (const MaterialTextureSlots& slots : materialTextureSlots)
    {
        bindings.push_back(MaterialTextureBinding{
            { textures[slots.baseColor]->GetImageView(), textures[slots.baseColor]->GetSampler() },
            { textures[slots.normal]->GetImageView(), textures[slots.normal]->GetSampler() },
            { textures[slots.metallic]->GetImageView(), textures[slots.metallic]->GetSampler() },
            { textures[slots.roughness]->GetImageView(), textures[slots.roughness]->GetSampler() },
            { textures[slots.occlusion]->GetImageView(), textures[slots.occlusion]->GetSampler() },
            { textures[slots.emissive]->GetImageView(), textures[slots.emissive]->GetSampler() }
        });
    }

    return bindings;
}

void LogVulkanRuntimeInfo()
{
    uint32_t apiVersion = 0;
    CheckVulkan(vkEnumerateInstanceVersion(&apiVersion), "Failed to query Vulkan runtime version");
    LOG_INFO(
        "Vulkan runtime API version: {}.{}.{}",
        VK_API_VERSION_MAJOR(apiVersion),
        VK_API_VERSION_MINOR(apiVersion),
        VK_API_VERSION_PATCH(apiVersion)
    );
}

void UpdateCameraFromInput(Camera& camera, const InputState& input, float deltaTime, bool blockKeyboardInput)
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
}
}

VulkanRenderer::VulkanRenderer(Window& window, std::optional<std::string> startupModelPath)
    : m_window(window),
      m_pendingModelPath(std::move(startupModelPath))
{
    LogVulkanRuntimeInfo();
    InitializeEditorScene();

    m_instance = std::make_unique<VulkanInstance>(m_window.GetSDLWindow());
    m_device = std::make_unique<VulkanDevice>(m_instance->GetHandle(), m_instance->GetSurface());
    m_editorScene.CreateTwoCubeTestScene();
    RebuildSceneRenderables();

    m_imguiLayer = std::make_unique<VulkanImGuiLayer>(
        m_window.GetSDLWindow(),
        m_instance->GetHandle(),
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        m_device->GetGraphicsQueue()
    );
    CreateSwapchainResources();
    CreatePipelineResources();
}

VulkanRenderer::~VulkanRenderer()
{
    if (m_device)
    {
        vkDeviceWaitIdle(m_device->GetHandle());
    }

    DestroyPipelineResources();
    DestroySwapchainResources();
    m_imguiLayer.reset();
    m_textures.clear();
    m_renderSubmeshes.clear();
    m_device.reset();
    m_instance.reset();
}

void VulkanRenderer::HandleEvent(const SDL_Event& event)
{
    m_input.HandleEvent(event);
    m_imguiLayer->ProcessEvent(event);

    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        event.button.button == SDL_BUTTON_RIGHT)
    {
        SDL_SetWindowRelativeMouseMode(m_window.GetSDLWindow(), event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    }
}

void VulkanRenderer::DrawFrame()
{
    const auto currentFrameTime = std::chrono::steady_clock::now();
    const float deltaTime = std::chrono::duration<float>(currentFrameTime - m_lastFrameTime).count();
    m_lastFrameTime = currentFrameTime;

    UpdateCameraFromInput(m_camera, m_input, deltaTime, m_imguiLayer->WantsKeyboardCapture());
    m_input.EndFrame();

    if (!HasDrawableArea())
    {
        return;
    }

    ProcessPendingSceneLoad();
    ProcessPendingModelLoad();
    UpdateViewportMatrices(m_swapchain->GetExtent());

    m_imguiLayer->BeginFrame();
    const std::string selectedModelPath =
        m_editorScene.HasSelection() ? m_editorScene.GetSelectedModel().sourcePath : std::string{};
    if (const EditorUiActions uiActions = m_imguiLayer->DrawEditorUi(
            m_camera,
            m_viewportMatrices,
            m_editorScene,
            selectedModelPath,
            m_lastModelLoadError,
            m_lastSceneIoError,
            m_swapchain->GetExtent()
        );
        uiActions.selectedModelPath.has_value() ||
        uiActions.selectedSceneLoadPath.has_value() ||
        uiActions.selectedSceneSavePath.has_value())
    {
        if (uiActions.selectedModelPath.has_value())
        {
            m_pendingModelPath = *uiActions.selectedModelPath;
        }
        if (uiActions.selectedSceneLoadPath.has_value())
        {
            m_pendingScenePath = *uiActions.selectedSceneLoadPath;
        }
        if (uiActions.selectedSceneSavePath.has_value())
        {
            try
            {
                m_editorScene.SaveSceneToFile(*uiActions.selectedSceneSavePath);
                m_editorScene.SetSceneFilePath(*uiActions.selectedSceneSavePath);
                m_lastSceneIoError.clear();
                LOG_INFO("Saved scene successfully: {}", *uiActions.selectedSceneSavePath);
            }
            catch (const std::exception& error)
            {
                m_lastSceneIoError = error.what();
                LOG_ERROR("Failed to save scene '{}': {}", *uiActions.selectedSceneSavePath, error.what());
            }
        }
    }
    ImGui::Render();

    uint32_t imageIndex = 0;
    const VkResult acquireResult = m_commandContext->AcquireNextImage(m_swapchain->GetHandle(), imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        RecreateSwapchain();
        return;
    }

    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        CheckVulkan(acquireResult, "Failed to acquire swapchain image");
    }

    m_uniformBuffer->Update(imageIndex, m_viewportMatrices, m_camera.position);

    std::vector<VulkanDrawItem> drawItems;
    drawItems.reserve(m_renderSubmeshes.size());
    for (const RenderSubmesh& renderSubmesh : m_renderSubmeshes)
    {
        ObjectPushConstants drawConstants{};
        drawConstants.model = m_editorScene.GetModelMatrix(renderSubmesh.entity);
        drawConstants.material = renderSubmesh.material;
        drawItems.push_back(VulkanDrawItem{
            renderSubmesh.buffer->GetVertexHandle(),
            renderSubmesh.buffer->GetIndexHandle(),
            renderSubmesh.buffer->GetIndexCount(),
            m_uniformBuffer->GetDescriptorSet(imageIndex, renderSubmesh.materialBindingIndex),
            drawConstants
        });
    }

    m_commandContext->RecordCommandBuffer(
        imageIndex,
        m_renderPass->GetHandle(),
        m_renderPass->GetFramebuffers()[imageIndex],
        m_swapchain->GetExtent(),
        m_graphicsPipeline->GetHandle(),
        m_graphicsPipeline->GetLayout(),
        drawItems,
        [](VkCommandBuffer commandBuffer)
        {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
        }
    );
    m_commandContext->Submit(m_device->GetGraphicsQueue(), imageIndex);

    const VkResult presentResult = m_commandContext->Present(m_device->GetPresentQueue(), m_swapchain->GetHandle(), imageIndex);
    if (acquireResult == VK_SUBOPTIMAL_KHR || presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
    {
        RecreateSwapchain();
        return;
    }

    if (presentResult != VK_SUCCESS)
    {
        CheckVulkan(presentResult, "Failed to present swapchain image");
    }
}

void VulkanRenderer::CreateSwapchainResources()
{
    const SwapchainSupportDetails supportDetails = m_device->QuerySwapchainSupport();
    m_swapchain = std::make_unique<VulkanSwapchain>(
        m_window.GetSDLWindow(),
        m_device->GetHandle(),
        m_instance->GetSurface(),
        m_device->GetQueueFamilies(),
        supportDetails
    );
    m_renderPass = std::make_unique<VulkanRenderPass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_swapchain->GetImageFormat(),
        m_swapchain->GetExtent(),
        m_swapchain->GetImageViews()
    );
    m_commandContext = std::make_unique<VulkanCommandContext>(
        m_device->GetHandle(),
        m_device->GetQueueFamilies(),
        m_renderPass->GetFramebuffers().size()
    );
    m_imguiLayer->CreateOrUpdateVulkanResources(m_renderPass->GetHandle(), static_cast<uint32_t>(m_swapchain->GetImageViews().size()));
}

void VulkanRenderer::DestroySwapchainResources()
{
    if (m_imguiLayer)
    {
        m_imguiLayer->DestroyVulkanResources();
    }
    m_commandContext.reset();
    m_renderPass.reset();
    m_swapchain.reset();
}

void VulkanRenderer::CreatePipelineResources()
{
    if (m_textures.empty())
    {
        throw std::runtime_error("Cannot create pipeline resources without at least one texture");
    }
    if (m_materialTextureSlots.empty())
    {
        throw std::runtime_error("Cannot create pipeline resources without at least one material texture binding");
    }

    m_uniformBuffer = std::make_unique<VulkanUniformBuffer>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        static_cast<uint32_t>(m_swapchain->GetImageViews().size()),
        BuildMaterialTextureBindings(m_textures, m_materialTextureSlots)
    );
    m_graphicsPipeline = std::make_unique<VulkanPipeline>(
        m_device->GetHandle(),
        m_swapchain->GetExtent(),
        m_renderPass->GetHandle(),
        m_uniformBuffer->GetDescriptorSetLayout()
    );
}

void VulkanRenderer::DestroyPipelineResources()
{
    m_graphicsPipeline.reset();
    m_uniformBuffer.reset();
}

void VulkanRenderer::RecreateSwapchain()
{
    if (!HasDrawableArea())
    {
        return;
    }

    vkDeviceWaitIdle(m_device->GetHandle());
    DestroyPipelineResources();
    DestroySwapchainResources();
    CreateSwapchainResources();
    CreatePipelineResources();
}

bool VulkanRenderer::HasDrawableArea() const
{
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(m_window.GetSDLWindow(), &width, &height))
    {
        throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
    }

    return width > 0 && height > 0;
}

void VulkanRenderer::LoadSelectedModel(const std::string& path, bool resetTransform)
{
    if (!m_editorScene.HasSelection())
    {
        throw std::runtime_error("No selected entity available to receive the model");
    }

    entt::entity selectedEntity = m_editorScene.GetSelectedEntity();
    ModelComponent previousModel = m_editorScene.GetModel(selectedEntity);
    m_editorScene.GetModel(selectedEntity).sourcePath = path;
    m_editorScene.GetModel(selectedEntity).displayName = std::filesystem::path(path).filename().string();

    try
    {
        RebuildSceneRenderables();
    }
    catch (...)
    {
        m_editorScene.GetModel(selectedEntity) = previousModel;
        throw;
    }

    const ModelComponent& model = m_editorScene.GetModel(selectedEntity);
    if (model.hasBounds)
    {
        m_camera.FrameBounds(model.minBounds, model.maxBounds);
    }

    m_lastModelLoadError.clear();
    if (resetTransform)
    {
        m_editorScene.ResetSelectedTransform();
    }
    LOG_INFO("Loaded model successfully into '{}': {}", m_editorScene.GetTag(selectedEntity).name, path);
}

void VulkanRenderer::LoadScene(const std::string& path)
{
    const SerializedSceneData sceneData = EditorScene::LoadSceneDataFromFile(path);
    m_editorScene.ApplySceneData(sceneData);
    RebuildSceneRenderables();
    m_editorScene.SetSceneFilePath(path);
    m_lastSceneIoError.clear();
    LOG_INFO("Loaded scene successfully: {}", path);
}

void VulkanRenderer::RebuildSceneRenderables()
{
    std::vector<std::unique_ptr<VulkanTexture>> newTextures;
    std::vector<MaterialTextureSlots> newMaterialTextureSlots;
    std::vector<RenderSubmesh> newRenderSubmeshes;
    std::unordered_map<std::string, uint32_t> textureCache;

    auto getOrCreateTexture = [&](const std::string& cacheKey, const TextureData& textureData, VulkanTextureFormat textureFormat) -> uint32_t
    {
        if (const auto iterator = textureCache.find(cacheKey); iterator != textureCache.end())
        {
            return iterator->second;
        }

        const uint32_t textureIndex = static_cast<uint32_t>(newTextures.size());
        newTextures.push_back(std::make_unique<VulkanTexture>(
            m_device->GetPhysicalDevice(),
            m_device->GetHandle(),
            m_device->GetQueueFamilies().graphicsFamily.value(),
            m_device->GetGraphicsQueue(),
            textureData,
            textureFormat
        ));
        textureCache.emplace(cacheKey, textureIndex);
        return textureIndex;
    };

    const uint32_t defaultBaseColorIndex = getOrCreateTexture(
        "__default_base_color__",
        CreateSolidTexture(255, 255, 255, 255),
        VulkanTextureFormat::SrgbColor
    );
    const uint32_t defaultNormalIndex = getOrCreateTexture(
        "__default_normal__",
        CreateFlatNormalTexture(),
        VulkanTextureFormat::LinearData
    );
    const uint32_t defaultMetallicIndex = getOrCreateTexture(
        "__default_metallic__",
        CreateSolidTexture(255, 255, 255, 255),
        VulkanTextureFormat::LinearData
    );
    const uint32_t defaultRoughnessIndex = getOrCreateTexture(
        "__default_roughness__",
        CreateSolidTexture(255, 255, 255, 255),
        VulkanTextureFormat::LinearData
    );
    const uint32_t defaultOcclusionIndex = getOrCreateTexture(
        "__default_occlusion__",
        CreateSolidTexture(255, 255, 255, 255),
        VulkanTextureFormat::LinearData
    );
    const uint32_t defaultEmissiveIndex = getOrCreateTexture(
        "__default_emissive__",
        CreateSolidTexture(255, 255, 255, 255),
        VulkanTextureFormat::SrgbColor
    );

    const uint32_t defaultMaterialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
    newMaterialTextureSlots.push_back(MaterialTextureSlots{
        defaultBaseColorIndex,
        defaultNormalIndex,
        defaultMetallicIndex,
        defaultRoughnessIndex,
        defaultOcclusionIndex,
        defaultEmissiveIndex
    });

    auto loadTextureIndex = [&](const std::string& texturePath, VulkanTextureFormat textureFormat, uint32_t fallbackIndex, const std::string& materialName) -> uint32_t
    {
        if (texturePath.empty())
        {
            return fallbackIndex;
        }

        const std::string cacheKey = BuildTextureCacheKey(texturePath, textureFormat);
        if (const auto iterator = textureCache.find(cacheKey); iterator != textureCache.end())
        {
            return iterator->second;
        }

        try
        {
            const uint32_t textureIndex = static_cast<uint32_t>(newTextures.size());
            newTextures.push_back(std::make_unique<VulkanTexture>(
                m_device->GetPhysicalDevice(),
                m_device->GetHandle(),
                m_device->GetQueueFamilies().graphicsFamily.value(),
                m_device->GetGraphicsQueue(),
                texturePath,
                textureFormat
            ));
            textureCache.emplace(cacheKey, textureIndex);
            return textureIndex;
        }
        catch (const std::exception& error)
        {
            LOG_ERROR(
                "Failed to load model texture '{}' for material '{}': {}",
                texturePath,
                materialName,
                error.what()
            );
            return fallbackIndex;
        }
    };

    m_editorScene.ForEachEntity([&](
        entt::entity entity,
        const TagComponent& tag,
        const TransformComponent&,
        const ModelComponent& model
    )
    {
        if (model.sourcePath.empty())
        {
            RenderSubmesh renderSubmesh{};
            renderSubmesh.entity = entity;
            renderSubmesh.buffer = std::make_unique<VulkanBuffer>(
                m_device->GetPhysicalDevice(),
                m_device->GetHandle(),
                m_device->GetQueueFamilies().graphicsFamily.value(),
                m_device->GetGraphicsQueue()
            );
            renderSubmesh.materialBindingIndex = defaultMaterialBindingIndex;
            renderSubmesh.material = MaterialPushConstants{};
            renderSubmesh.name = tag.name;
            if (tag.name == "Cube A")
            {
                renderSubmesh.material.baseColorFactor[0] = 1.0f;
                renderSubmesh.material.baseColorFactor[1] = 0.55f;
                renderSubmesh.material.baseColorFactor[2] = 0.35f;
            }
            else if (tag.name == "Cube B")
            {
                renderSubmesh.material.baseColorFactor[0] = 0.35f;
                renderSubmesh.material.baseColorFactor[1] = 0.75f;
                renderSubmesh.material.baseColorFactor[2] = 1.0f;
            }
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
            return;
        }

        const LoadedModelData modelData = ModelLoader::LoadModel(model.sourcePath);
        std::vector<uint32_t> materialBindingIndices(modelData.materials.size(), 0);

        for (uint32_t materialIndex = 0; materialIndex < static_cast<uint32_t>(modelData.materials.size()); ++materialIndex)
        {
            const ModelMaterialData& material = modelData.materials[materialIndex];
            MaterialTextureSlots slots = newMaterialTextureSlots[defaultMaterialBindingIndex];
            slots.baseColor = loadTextureIndex(material.baseColorTexturePath, VulkanTextureFormat::SrgbColor, defaultBaseColorIndex, material.name);
            slots.normal = loadTextureIndex(material.normalTexturePath, VulkanTextureFormat::LinearData, defaultNormalIndex, material.name);
            slots.metallic = loadTextureIndex(material.metallicTexturePath, VulkanTextureFormat::LinearData, defaultMetallicIndex, material.name);
            slots.roughness = loadTextureIndex(material.roughnessTexturePath, VulkanTextureFormat::LinearData, defaultRoughnessIndex, material.name);
            slots.occlusion = loadTextureIndex(material.occlusionTexturePath, VulkanTextureFormat::LinearData, defaultOcclusionIndex, material.name);
            slots.emissive = loadTextureIndex(material.emissiveTexturePath, VulkanTextureFormat::SrgbColor, defaultEmissiveIndex, material.name);

            materialBindingIndices[materialIndex] = static_cast<uint32_t>(newMaterialTextureSlots.size());
            newMaterialTextureSlots.push_back(slots);
        }

        for (const ModelSubmeshData& submesh : modelData.submeshes)
        {
            RenderSubmesh renderSubmesh{};
            renderSubmesh.entity = entity;
            renderSubmesh.buffer = std::make_unique<VulkanBuffer>(
                m_device->GetPhysicalDevice(),
                m_device->GetHandle(),
                m_device->GetQueueFamilies().graphicsFamily.value(),
                m_device->GetGraphicsQueue(),
                submesh.mesh
            );
            renderSubmesh.materialBindingIndex =
                submesh.hasTexCoords ? materialBindingIndices[submesh.materialIndex] : defaultMaterialBindingIndex;

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
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
        }

        m_editorScene.UpdateModelInfo(
            entity,
            model.displayName,
            model.sourcePath,
            static_cast<uint32_t>(modelData.submeshes.size()),
            modelData.minBounds,
            modelData.maxBounds,
            modelData.hasBounds
        );
    });

    ApplyRenderContent(std::move(newTextures), std::move(newMaterialTextureSlots), std::move(newRenderSubmeshes));
}

void VulkanRenderer::ApplyRenderContent(
    std::vector<std::unique_ptr<VulkanTexture>> newTextures,
    std::vector<MaterialTextureSlots> newMaterialTextureSlots,
    std::vector<RenderSubmesh> newRenderSubmeshes
)
{
    std::unique_ptr<VulkanUniformBuffer> newUniformBuffer;
    std::unique_ptr<VulkanPipeline> newGraphicsPipeline;

    if (m_swapchain && m_renderPass)
    {
        newUniformBuffer = std::make_unique<VulkanUniformBuffer>(
            m_device->GetPhysicalDevice(),
            m_device->GetHandle(),
            static_cast<uint32_t>(m_swapchain->GetImageViews().size()),
            BuildMaterialTextureBindings(newTextures, newMaterialTextureSlots)
        );
        newGraphicsPipeline = std::make_unique<VulkanPipeline>(
            m_device->GetHandle(),
            m_swapchain->GetExtent(),
            m_renderPass->GetHandle(),
            newUniformBuffer->GetDescriptorSetLayout()
        );
        vkDeviceWaitIdle(m_device->GetHandle());
        m_uniformBuffer = std::move(newUniformBuffer);
        m_graphicsPipeline = std::move(newGraphicsPipeline);
    }

    m_textures = std::move(newTextures);
    m_materialTextureSlots = std::move(newMaterialTextureSlots);
    m_renderSubmeshes = std::move(newRenderSubmeshes);
}

void VulkanRenderer::ProcessPendingModelLoad()
{
    if (!m_pendingModelPath.has_value())
    {
        return;
    }

    const std::string path = *m_pendingModelPath;
    m_pendingModelPath.reset();

    try
    {
        LOG_INFO("Loading model: {}", path);
        LoadSelectedModel(path);
    }
    catch (const std::exception& error)
    {
        m_lastModelLoadError = error.what();
        LOG_ERROR("Failed to load model '{}': {}", path, error.what());
    }

    m_lastFrameTime = std::chrono::steady_clock::now();
}

void VulkanRenderer::ProcessPendingSceneLoad()
{
    if (!m_pendingScenePath.has_value())
    {
        return;
    }

    const std::string path = *m_pendingScenePath;
    m_pendingScenePath.reset();

    try
    {
        LoadScene(path);
    }
    catch (const std::exception& error)
    {
        m_lastSceneIoError = error.what();
        LOG_ERROR("Failed to load scene '{}': {}", path, error.what());
    }

    m_lastFrameTime = std::chrono::steady_clock::now();
}

void VulkanRenderer::InitializeEditorScene()
{
    m_editorScene.LoadConfig(MINIENGINE_ASSET_DIR "/editor/default_scene.yaml");
}

void VulkanRenderer::UpdateViewportMatrices(VkExtent2D extent)
{
    m_viewportMatrices.view = m_camera.GetViewMatrix();
    m_viewportMatrices.projection = m_camera.GetProjectionMatrix(extent, false);
    m_viewportMatrices.renderProjection = m_camera.GetProjectionMatrix(extent, true);
    m_viewportMatrices.model =
        m_editorScene.HasSelection() ? m_editorScene.GetModelMatrix(m_editorScene.GetSelectedEntity()) : glm::mat4(1.0f);
}
