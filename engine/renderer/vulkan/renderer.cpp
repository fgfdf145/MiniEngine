#include "renderer.h"

#include "../imgui/imgui_impl_vulkan.h"

#include <imgui.h>
#include <log/log.h>
#include <window/window.h>

#include <array>
#include <cstdint>
#include <unordered_map>

namespace
{
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

VkExtent2D ToVkExtent(RenderExtent extent)
{
    return VkExtent2D{
        std::max(extent.width, 1u),
        std::max(extent.height, 1u)
    };
}

RenderExtent FromVkExtent(VkExtent2D extent)
{
    return RenderExtent{
        extent.width,
        extent.height
    };
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
}

VulkanRenderer::VulkanRenderer(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    std::optional<std::string> startupModelPath
)
    : EditorRenderBackendBase(window, std::move(sharedState), RenderBackendType::Vulkan, std::move(startupModelPath))
{
    LogVulkanRuntimeInfo();

    m_instance = std::make_unique<VulkanInstance>(GetWindow().GetSDLWindow());
    m_device = std::make_unique<VulkanDevice>(m_instance->GetHandle(), m_instance->GetSurface());
    m_imguiLayer = std::make_unique<VulkanImGuiLayer>(
        GetWindow().GetSDLWindow(),
        m_instance->GetHandle(),
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        m_device->GetGraphicsQueue()
    );
    CreateSwapchainResources();
    UploadSceneResources();
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

void VulkanRenderer::DrawFrame()
{
    if (!TickSharedFrame())
    {
        return;
    }

    if (ProcessPendingOperations())
    {
        UploadSceneResources();
    }

    SyncSceneViewportLayer();

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

    UpdateViewportMatrices(FromVkExtent(m_sceneViewportLayer->GetExtent()));

    m_imguiLayer->BeginFrame();
    State().editorUi.BeginFrame(GetWindow().GetSDLWindow());
    const EditorUiFrameResult uiFrame = DrawEditorUi(
        m_sceneViewportLayer->GetTextureId(imageIndex),
        FromVkExtent(m_sceneViewportLayer->GetExtent())
    );
    ApplyUiActions(uiFrame);
    ImGui::Render();

    m_uniformBuffer->Update(imageIndex, State().viewportMatrices, State().camera.position);
    const std::vector<VulkanDrawItem> drawItems = BuildDrawItems(imageIndex);
    m_commandContext->RecordCommandBuffer(imageIndex, [&](VkCommandBuffer commandBuffer)
    {
        RecordSceneLayer(commandBuffer, imageIndex, drawItems);
        RecordEditorLayer(commandBuffer, imageIndex);
    });
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

void VulkanRenderer::HandleBackendEvent(const SDL_Event& event)
{
    m_imguiLayer->ProcessEvent(event);
}

bool VulkanRenderer::WantsKeyboardCapture() const
{
    return m_imguiLayer->WantsKeyboardCapture();
}

void VulkanRenderer::CreateSwapchainResources()
{
    const SwapchainSupportDetails supportDetails = m_device->QuerySwapchainSupport();
    m_swapchain = std::make_unique<VulkanSwapchain>(
        GetWindow().GetSDLWindow(),
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
    if (!State().requestedViewportExtent.IsValid())
    {
        State().requestedViewportExtent = FromVkExtent(m_swapchain->GetExtent());
    }
    m_sceneViewportLayer = std::make_unique<VulkanSceneViewport>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_swapchain->GetImageFormat(),
        ToVkExtent(State().requestedViewportExtent),
        static_cast<uint32_t>(m_swapchain->GetImageViews().size())
    );
}

void VulkanRenderer::DestroySwapchainResources()
{
    m_sceneViewportLayer.reset();
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
        m_sceneViewportLayer->GetExtent(),
        m_sceneViewportLayer->GetRenderPass(),
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

void VulkanRenderer::SyncSceneViewportLayer()
{
    if (!m_swapchain || !m_sceneViewportLayer)
    {
        return;
    }

    if (!State().requestedViewportExtent.IsValid())
    {
        State().requestedViewportExtent = FromVkExtent(m_sceneViewportLayer->GetExtent());
    }

    if (m_sceneViewportLayer->MatchesExtent(ToVkExtent(State().requestedViewportExtent)))
    {
        return;
    }

    vkDeviceWaitIdle(m_device->GetHandle());
    DestroyPipelineResources();
    m_sceneViewportLayer = std::make_unique<VulkanSceneViewport>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_swapchain->GetImageFormat(),
        ToVkExtent(State().requestedViewportExtent),
        static_cast<uint32_t>(m_swapchain->GetImageViews().size())
    );
    CreatePipelineResources();
}

void VulkanRenderer::UploadSceneResources()
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

    auto loadTextureIndex = [&](const std::string& texturePath, VulkanTextureFormat textureFormat, uint32_t fallbackIndex) -> uint32_t
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
            LOG_ERROR("Failed to load model texture '{}': {}", texturePath, error.what());
            return fallbackIndex;
        }
    };

    for (const CpuRenderSubmesh& cpuRenderSubmesh : State().renderSubmeshes)
    {
        RenderSubmesh renderSubmesh{};
        renderSubmesh.entity = cpuRenderSubmesh.entity;
        renderSubmesh.buffer = std::make_unique<VulkanBuffer>(
            m_device->GetPhysicalDevice(),
            m_device->GetHandle(),
            m_device->GetQueueFamilies().graphicsFamily.value(),
            m_device->GetGraphicsQueue(),
            cpuRenderSubmesh.mesh
        );
        renderSubmesh.material = cpuRenderSubmesh.material;
        renderSubmesh.name = cpuRenderSubmesh.name;

        if (!cpuRenderSubmesh.hasTexCoords)
        {
            renderSubmesh.materialBindingIndex = defaultMaterialBindingIndex;
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
            continue;
        }

        MaterialTextureSlots slots = newMaterialTextureSlots[defaultMaterialBindingIndex];
        slots.baseColor = loadTextureIndex(cpuRenderSubmesh.textures.baseColor, VulkanTextureFormat::SrgbColor, defaultBaseColorIndex);
        slots.normal = loadTextureIndex(cpuRenderSubmesh.textures.normal, VulkanTextureFormat::LinearData, defaultNormalIndex);
        slots.metallic = loadTextureIndex(cpuRenderSubmesh.textures.metallic, VulkanTextureFormat::LinearData, defaultMetallicIndex);
        slots.roughness = loadTextureIndex(cpuRenderSubmesh.textures.roughness, VulkanTextureFormat::LinearData, defaultRoughnessIndex);
        slots.occlusion = loadTextureIndex(cpuRenderSubmesh.textures.occlusion, VulkanTextureFormat::LinearData, defaultOcclusionIndex);
        slots.emissive = loadTextureIndex(cpuRenderSubmesh.textures.emissive, VulkanTextureFormat::SrgbColor, defaultEmissiveIndex);

        renderSubmesh.materialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
        newMaterialTextureSlots.push_back(slots);
        newRenderSubmeshes.push_back(std::move(renderSubmesh));
    }

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

    if (m_swapchain && m_renderPass && m_sceneViewportLayer && !newTextures.empty() && !newMaterialTextureSlots.empty())
    {
        newUniformBuffer = std::make_unique<VulkanUniformBuffer>(
            m_device->GetPhysicalDevice(),
            m_device->GetHandle(),
            static_cast<uint32_t>(m_swapchain->GetImageViews().size()),
            BuildMaterialTextureBindings(newTextures, newMaterialTextureSlots)
        );
        newGraphicsPipeline = std::make_unique<VulkanPipeline>(
            m_device->GetHandle(),
            m_sceneViewportLayer->GetExtent(),
            m_sceneViewportLayer->GetRenderPass(),
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

std::vector<VulkanDrawItem> VulkanRenderer::BuildDrawItems(uint32_t imageIndex) const
{
    std::vector<VulkanDrawItem> drawItems;
    drawItems.reserve(m_renderSubmeshes.size());

    for (const RenderSubmesh& renderSubmesh : m_renderSubmeshes)
    {
        ObjectPushConstants drawConstants{};
        drawConstants.model = State().editorScene.GetModelMatrix(renderSubmesh.entity);
        drawConstants.material = renderSubmesh.material;
        drawItems.push_back(VulkanDrawItem{
            renderSubmesh.buffer->GetVertexHandle(),
            renderSubmesh.buffer->GetIndexHandle(),
            renderSubmesh.buffer->GetIndexCount(),
            m_uniformBuffer->GetDescriptorSet(imageIndex, renderSubmesh.materialBindingIndex),
            drawConstants
        });
    }

    return drawItems;
}

void VulkanRenderer::RecordSceneLayer(
    VkCommandBuffer commandBuffer,
    uint32_t imageIndex,
    const std::vector<VulkanDrawItem>& drawItems
) const
{
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = { { 0.08f, 0.1f, 0.16f, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_sceneViewportLayer->GetRenderPass();
    renderPassInfo.framebuffer = m_sceneViewportLayer->GetFramebuffer(imageIndex);
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = m_sceneViewportLayer->GetExtent();
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline->GetHandle());

    for (const VulkanDrawItem& drawItem : drawItems)
    {
        const VkBuffer vertexBuffers[] = { drawItem.vertexBuffer };
        const VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, drawItem.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_graphicsPipeline->GetLayout(),
            0,
            1,
            &drawItem.descriptorSet,
            0,
            nullptr
        );
        vkCmdPushConstants(
            commandBuffer,
            m_graphicsPipeline->GetLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(ObjectPushConstants),
            &drawItem.drawConstants
        );
        vkCmdDrawIndexed(commandBuffer, drawItem.indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);
}

void VulkanRenderer::RecordEditorLayer(VkCommandBuffer commandBuffer, uint32_t imageIndex) const
{
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = { { 0.04f, 0.05f, 0.08f, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass->GetHandle();
    renderPassInfo.framebuffer = m_renderPass->GetFramebuffers()[imageIndex];
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = m_swapchain->GetExtent();
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    vkCmdEndRenderPass(commandBuffer);
}
