#include "renderer.h"

#include "../imgui/imgui_impl_vulkan.h"
#include "../renderer_shared_state.h"

#include <editor_world.h>
#include <scene_components.h>
#include <imgui.h>
#include <log/log.h>
#include <window/window.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <execution>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
// Build a direction vector from a transform's Euler rotation (XYZ order, same as BuildTransformMatrix).
glm::vec3 BuildLightDirection(const TransformComponent& transform)
{
    glm::mat4 rotMat(1.0f);
    rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
    rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
    rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::normalize(glm::vec3(rotMat * glm::vec4(0.0f, -1.0f, 0.0f, 0.0f)));
}

std::vector<GpuLightData> CollectSceneLights(const IEditorWorld& world)
{
    std::vector<GpuLightData> gpuLights;
    world.ForEachLight([&](
        entt::entity,
        const TagComponent&,
        const TransformComponent& transform,
        const LightComponent& light
    )
    {
        GpuLightData gpu{};
        gpu.positionAndRange = glm::vec4(transform.translation, light.range);
        gpu.colorAndIntensity = glm::vec4(light.color, light.intensity);

        const glm::vec3 direction = BuildLightDirection(transform);
        gpu.directionAndType = glm::vec4(direction, static_cast<float>(light.type));

        const float innerCos = std::cos(glm::radians(light.spotInnerAngleDegrees));
        const float outerCos = std::cos(glm::radians(light.spotOuterAngleDegrees));
        gpu.spotAndArea = glm::vec4(innerCos, outerCos, light.areaSize.x, light.areaSize.y);

        gpuLights.push_back(gpu);
    });
    return gpuLights;
}

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
            { textures[slots.emissive]->GetImageView(), textures[slots.emissive]->GetSampler() },
            { textures[slots.secondaryBaseColor]->GetImageView(), textures[slots.secondaryBaseColor]->GetSampler() },
            { textures[slots.secondaryNormal]->GetImageView(), textures[slots.secondaryNormal]->GetSampler() },
            { textures[slots.secondaryMetallic]->GetImageView(), textures[slots.secondaryMetallic]->GetSampler() },
            { textures[slots.secondaryRoughness]->GetImageView(), textures[slots.secondaryRoughness]->GetSampler() },
            { textures[slots.secondaryOcclusion]->GetImageView(), textures[slots.secondaryOcclusion]->GetSampler() },
            { textures[slots.secondaryEmissive]->GetImageView(), textures[slots.secondaryEmissive]->GetSampler() },
            { textures[slots.blendMask]->GetImageView(), textures[slots.blendMask]->GetSampler() }
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
    State().editorUi.BeginFrame(GetWindow().GetSDLWindow(), State().engineSettings);
    const EditorUiFrameResult uiFrame = DrawEditorUi(
        m_sceneViewportLayer->GetTextureId(imageIndex),
        FromVkExtent(m_sceneViewportLayer->GetExtent())
    );
    ApplyUiActions(uiFrame);
    if (State().renderablesDirty)
    {
        UploadSceneResources();
        State().renderablesDirty = false;
    }
    ImGui::Render();

    const std::vector<GpuLightData> gpuLights =
        State().editorWorld ? CollectSceneLights(*State().editorWorld) : std::vector<GpuLightData>{};
    m_uniformBuffer->Update(imageIndex, State().viewportMatrices, State().camera.position, gpuLights);
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
    m_graphicsPipelineDoubleSided = std::make_unique<VulkanPipeline>(
        m_device->GetHandle(),
        m_sceneViewportLayer->GetExtent(),
        m_sceneViewportLayer->GetRenderPass(),
        m_uniformBuffer->GetDescriptorSetLayout(),
        true
    );
}

void VulkanRenderer::DestroyPipelineResources()
{
    m_graphicsPipelineDoubleSided.reset();
    m_graphicsPipeline.reset();
    m_uniformBuffer.reset();
    // The pool holds VulkanTexture objects that reference VkImage/VkSampler; clear it whenever
    // the pipeline is torn down so nothing outlives the logical device.
    m_texturePool.clear();
    m_textureCacheKeys.clear();
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
    // Move live textures into the pool so they can be reused without re-uploading.
    // This prevents 2× peak VRAM usage when rebuilding an unchanged texture set.
    for (size_t i = 0; i < m_textures.size() && i < m_textureCacheKeys.size(); ++i)
    {
        if (!m_textureCacheKeys[i].empty() && m_textures[i])
        {
            m_texturePool.emplace(m_textureCacheKeys[i], std::move(m_textures[i]));
        }
    }
    m_textures.clear();
    m_textureCacheKeys.clear();

    std::vector<std::unique_ptr<VulkanTexture>> newTextures;
    std::vector<std::string>                    newCacheKeys;
    std::vector<MaterialTextureSlots>           newMaterialTextureSlots;
    std::vector<RenderSubmesh>                  newRenderSubmeshes;
    std::unordered_map<std::string, uint32_t>   keyToIndex;

    // Batch every texture and submesh-buffer upload below into a handful of submit+wait
    // rounds instead of one per resource: VulkanTexture/VulkanBuffer used to each own their
    // upload (command pool, submit, vkQueueWaitIdle), which serializes hundreds of GPU
    // round-trips in a row for models with many submeshes/textures (e.g. Sponza: 405 submeshes,
    // up to ~170 unique textures). Flushing periodically bounds how much staging memory is held
    // at once while still cutting the number of GPU stalls by roughly two orders of magnitude.
    constexpr size_t kResourcesPerUploadFlush = 64;
    VulkanUploadBatch uploadBatch(
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        m_device->GetGraphicsQueue()
    );
    size_t resourcesSinceFlush = 0;
    auto flushUploadBatchIfNeeded = [&]()
    {
        if (++resourcesSinceFlush >= kResourcesPerUploadFlush)
        {
            uploadBatch.Flush();
            resourcesSinceFlush = 0;
        }
    };

    // Acquire a texture by cache key: reuse from pool when available, else use createFn().
    // Returns UINT32_MAX on failure (createFn threw), caller should use fallback.
    auto acquireDefault = [&](const std::string& id, const TextureData& data, VulkanTextureFormat fmt) -> uint32_t
    {
        const std::string key = id + (fmt == VulkanTextureFormat::SrgbColor ? "|srgb" : "|linear");
        if (auto it = keyToIndex.find(key); it != keyToIndex.end())
            return it->second;

        const uint32_t idx = static_cast<uint32_t>(newTextures.size());
        if (auto poolIt = m_texturePool.find(key); poolIt != m_texturePool.end())
        {
            newTextures.push_back(std::move(poolIt->second));
            m_texturePool.erase(poolIt);
        }
        else
        {
            newTextures.push_back(std::make_unique<VulkanTexture>(
                m_device->GetPhysicalDevice(), m_device->GetHandle(),
                data, uploadBatch, fmt
            ));
            flushUploadBatchIfNeeded();
        }
        newCacheKeys.push_back(key);
        keyToIndex.emplace(key, idx);
        return idx;
    };

    auto loadTextureIndex = [&](const std::string& texturePath, VulkanTextureFormat textureFormat, uint32_t fallbackIndex) -> uint32_t
    {
        if (texturePath.empty()) return fallbackIndex;

        const std::string key = BuildTextureCacheKey(texturePath, textureFormat);
        if (auto it = keyToIndex.find(key); it != keyToIndex.end())
            return it->second;

        // Pool hit: reuse existing GPU texture — no disk I/O, no upload, no extra VRAM.
        if (auto poolIt = m_texturePool.find(key); poolIt != m_texturePool.end())
        {
            const uint32_t idx = static_cast<uint32_t>(newTextures.size());
            newTextures.push_back(std::move(poolIt->second));
            m_texturePool.erase(poolIt);
            newCacheKeys.push_back(key);
            keyToIndex.emplace(key, idx);
            return idx;
        }

        // Pool miss: load from disk and upload.
        try
        {
            const uint32_t idx = static_cast<uint32_t>(newTextures.size());
            newTextures.push_back(std::make_unique<VulkanTexture>(
                m_device->GetPhysicalDevice(), m_device->GetHandle(),
                texturePath, uploadBatch, textureFormat
            ));
            flushUploadBatchIfNeeded();
            newCacheKeys.push_back(key);
            keyToIndex.emplace(key, idx);
            return idx;
        }
        catch (const std::exception& error)
        {
            LOG_ERROR("Failed to load model texture '{}': {}", texturePath, error.what());
            return fallbackIndex;
        }
    };

    const uint32_t defaultBaseColorIndex  = acquireDefault("__default_base_color__",  CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::SrgbColor);
    const uint32_t defaultNormalIndex     = acquireDefault("__default_normal__",       CreateFlatNormalTexture(),               VulkanTextureFormat::LinearData);
    const uint32_t defaultMetallicIndex   = acquireDefault("__default_metallic__",     CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
    const uint32_t defaultRoughnessIndex  = acquireDefault("__default_roughness__",    CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
    const uint32_t defaultOcclusionIndex  = acquireDefault("__default_occlusion__",    CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
    const uint32_t defaultEmissiveIndex   = acquireDefault("__default_emissive__",     CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::SrgbColor);
    const uint32_t defaultBlendMaskIndex  = acquireDefault("__default_blend_mask__",   CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);

    const uint32_t defaultMaterialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
    newMaterialTextureSlots.push_back(MaterialTextureSlots{
        defaultBaseColorIndex, defaultNormalIndex,   defaultMetallicIndex,  defaultRoughnessIndex,
        defaultOcclusionIndex, defaultEmissiveIndex,
        defaultBaseColorIndex, defaultNormalIndex,   defaultMetallicIndex,  defaultRoughnessIndex,
        defaultOcclusionIndex, defaultEmissiveIndex, defaultBlendMaskIndex
    });

    // Prefetch: decode every not-yet-cached material texture file in parallel before the main
    // loop below loads them one at a time on this thread. Some source assets (e.g. Sponza) ship
    // 40+ MB source PNGs that take real CPU time to decode, and stb_image's per-call state is
    // thread_local in this build, so concurrent decodes from different threads are safe. This
    // only warms keyToIndex/newTextures; the main loop's loadTextureIndex() below is unchanged
    // and transparently picks up the prefetched entries via its existing cache-hit check, so any
    // texture this pass misses (or fails to decode) just falls back to loading inline as before.
    {
        struct PendingTextureLoad
        {
            std::string cacheKey;
            std::string path;
            VulkanTextureFormat format;
            TextureData decoded;
            bool decodeFailed = false;
            std::string decodeError;
        };

        std::vector<PendingTextureLoad> pendingLoads;
        std::unordered_set<std::string> seenKeys;
        auto considerPath = [&](const std::string& path, VulkanTextureFormat format)
        {
            if (path.empty())
            {
                return;
            }
            const std::string key = BuildTextureCacheKey(path, format);
            if (m_texturePool.count(key) != 0 || !seenKeys.insert(key).second)
            {
                return;
            }
            pendingLoads.push_back(PendingTextureLoad{ key, path, format, {}, false, {} });
        };

        for (const CpuRenderSubmesh& cpuRenderSubmesh : State().rendererWorld.GetRenderSubmeshes())
        {
            if (!cpuRenderSubmesh.hasTexCoords)
            {
                continue;
            }
            const MaterialTexturePaths& textures = cpuRenderSubmesh.textures;
            considerPath(textures.baseColor,          VulkanTextureFormat::SrgbColor);
            considerPath(textures.normal,             VulkanTextureFormat::LinearData);
            considerPath(textures.metallic,           VulkanTextureFormat::LinearData);
            considerPath(textures.roughness,          VulkanTextureFormat::LinearData);
            considerPath(textures.occlusion,          VulkanTextureFormat::LinearData);
            considerPath(textures.emissive,           VulkanTextureFormat::SrgbColor);
            considerPath(textures.secondaryBaseColor, VulkanTextureFormat::SrgbColor);
            considerPath(textures.secondaryNormal,    VulkanTextureFormat::LinearData);
            considerPath(textures.secondaryMetallic,  VulkanTextureFormat::LinearData);
            considerPath(textures.secondaryRoughness, VulkanTextureFormat::LinearData);
            considerPath(textures.secondaryOcclusion, VulkanTextureFormat::LinearData);
            considerPath(textures.secondaryEmissive,  VulkanTextureFormat::SrgbColor);
            considerPath(textures.blendMask,          VulkanTextureFormat::LinearData);
        }

        // Decode in bounded chunks rather than all at once, so we don't hold dozens of huge
        // decoded RGBA8 buffers in host memory simultaneously (a 40 MB source PNG can decode to
        // 100+ MB of raw pixels).
        const size_t chunkSize = std::max<size_t>(8, static_cast<size_t>(std::thread::hardware_concurrency()) * 2);
        for (size_t chunkStart = 0; chunkStart < pendingLoads.size(); chunkStart += chunkSize)
        {
            const size_t chunkEnd = std::min(chunkStart + chunkSize, pendingLoads.size());

            std::for_each(
                std::execution::par,
                pendingLoads.begin() + static_cast<std::ptrdiff_t>(chunkStart),
                pendingLoads.begin() + static_cast<std::ptrdiff_t>(chunkEnd),
                [](PendingTextureLoad& pending)
                {
                    try
                    {
                        pending.decoded = TextureLoader::LoadRGBA8(pending.path);
                    }
                    catch (const std::exception& error)
                    {
                        pending.decodeFailed = true;
                        pending.decodeError = error.what();
                    }
                }
            );

            for (size_t i = chunkStart; i < chunkEnd; ++i)
            {
                PendingTextureLoad& pending = pendingLoads[i];
                if (pending.decodeFailed)
                {
                    LOG_ERROR("Failed to prefetch model texture '{}': {}", pending.path, pending.decodeError);
                    continue;
                }

                try
                {
                    const uint32_t idx = static_cast<uint32_t>(newTextures.size());
                    newTextures.push_back(std::make_unique<VulkanTexture>(
                        m_device->GetPhysicalDevice(), m_device->GetHandle(),
                        pending.decoded, uploadBatch, pending.format
                    ));
                    flushUploadBatchIfNeeded();
                    newCacheKeys.push_back(pending.cacheKey);
                    keyToIndex.emplace(pending.cacheKey, idx);
                }
                catch (const std::exception& error)
                {
                    LOG_ERROR("Failed to upload prefetched model texture '{}': {}", pending.path, error.what());
                }

                // Release the decoded pixels promptly instead of waiting for pendingLoads itself
                // to go out of scope at the end of the prefetch block.
                pending.decoded.pixels.clear();
                pending.decoded.pixels.shrink_to_fit();
            }
        }
    }

    for (const CpuRenderSubmesh& cpuRenderSubmesh : State().rendererWorld.GetRenderSubmeshes())
    {
        RenderSubmesh renderSubmesh{};
        renderSubmesh.entity   = cpuRenderSubmesh.entity;
        renderSubmesh.buffer   = std::make_unique<VulkanBuffer>(
            m_device->GetPhysicalDevice(), m_device->GetHandle(),
            cpuRenderSubmesh.mesh, uploadBatch
        );
        flushUploadBatchIfNeeded();
        renderSubmesh.material = cpuRenderSubmesh.material;
        renderSubmesh.doubleSided = cpuRenderSubmesh.doubleSided;
        renderSubmesh.name     = cpuRenderSubmesh.name;

        if (!cpuRenderSubmesh.hasTexCoords)
        {
            renderSubmesh.materialBindingIndex = defaultMaterialBindingIndex;
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
            continue;
        }

        MaterialTextureSlots slots = newMaterialTextureSlots[defaultMaterialBindingIndex];
        slots.baseColor        = loadTextureIndex(cpuRenderSubmesh.textures.baseColor,        VulkanTextureFormat::SrgbColor,  defaultBaseColorIndex);
        slots.normal           = loadTextureIndex(cpuRenderSubmesh.textures.normal,           VulkanTextureFormat::LinearData, defaultNormalIndex);
        slots.metallic         = loadTextureIndex(cpuRenderSubmesh.textures.metallic,         VulkanTextureFormat::LinearData, defaultMetallicIndex);
        slots.roughness        = loadTextureIndex(cpuRenderSubmesh.textures.roughness,        VulkanTextureFormat::LinearData, defaultRoughnessIndex);
        slots.occlusion        = loadTextureIndex(cpuRenderSubmesh.textures.occlusion,        VulkanTextureFormat::LinearData, defaultOcclusionIndex);
        slots.emissive         = loadTextureIndex(cpuRenderSubmesh.textures.emissive,         VulkanTextureFormat::SrgbColor,  defaultEmissiveIndex);
        slots.secondaryBaseColor  = loadTextureIndex(cpuRenderSubmesh.textures.secondaryBaseColor,  VulkanTextureFormat::SrgbColor,  slots.baseColor);
        slots.secondaryNormal     = loadTextureIndex(cpuRenderSubmesh.textures.secondaryNormal,     VulkanTextureFormat::LinearData, slots.normal);
        slots.secondaryMetallic   = loadTextureIndex(cpuRenderSubmesh.textures.secondaryMetallic,   VulkanTextureFormat::LinearData, slots.metallic);
        slots.secondaryRoughness  = loadTextureIndex(cpuRenderSubmesh.textures.secondaryRoughness,  VulkanTextureFormat::LinearData, slots.roughness);
        slots.secondaryOcclusion  = loadTextureIndex(cpuRenderSubmesh.textures.secondaryOcclusion,  VulkanTextureFormat::LinearData, slots.occlusion);
        slots.secondaryEmissive   = loadTextureIndex(cpuRenderSubmesh.textures.secondaryEmissive,   VulkanTextureFormat::SrgbColor,  slots.emissive);
        slots.blendMask           = loadTextureIndex(cpuRenderSubmesh.textures.blendMask,           VulkanTextureFormat::LinearData, defaultBlendMaskIndex);

        renderSubmesh.materialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
        newMaterialTextureSlots.push_back(slots);
        newRenderSubmeshes.push_back(std::move(renderSubmesh));
    }
    uploadBatch.Flush();
    LOG_INFO("Uploaded {} submesh buffers and {} textures", newRenderSubmeshes.size(), newTextures.size());

    // Textures remaining in the pool are no longer referenced; destroy them now.
    m_texturePool.clear();

    ApplyRenderContent(std::move(newTextures), std::move(newMaterialTextureSlots), std::move(newRenderSubmeshes));

    // Track the cache keys for the textures now in m_textures, so the next upload can pool them.
    m_textureCacheKeys = std::move(newCacheKeys);
}

void VulkanRenderer::ApplyRenderContent(
    std::vector<std::unique_ptr<VulkanTexture>> newTextures,
    std::vector<MaterialTextureSlots> newMaterialTextureSlots,
    std::vector<RenderSubmesh> newRenderSubmeshes
)
{
    std::unique_ptr<VulkanUniformBuffer> newUniformBuffer;
    std::unique_ptr<VulkanPipeline> newGraphicsPipeline;
    std::unique_ptr<VulkanPipeline> newGraphicsPipelineDoubleSided;

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
        newGraphicsPipelineDoubleSided = std::make_unique<VulkanPipeline>(
            m_device->GetHandle(),
            m_sceneViewportLayer->GetExtent(),
            m_sceneViewportLayer->GetRenderPass(),
            newUniformBuffer->GetDescriptorSetLayout(),
            true
        );
        // Wait only for our in-flight render frames to finish before destroying old resources.
        // vkWaitForFences is more targeted than vkDeviceWaitIdle: it doesn't stall the
        // present or transfer queues, and new UBO/pipeline above are built while the GPU
        // may still be executing the previous frame (overlapping CPU and GPU work).
        m_commandContext->WaitForAllFrames();
        m_uniformBuffer = std::move(newUniformBuffer);
        m_graphicsPipeline = std::move(newGraphicsPipeline);
        m_graphicsPipelineDoubleSided = std::move(newGraphicsPipelineDoubleSided);
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
        drawConstants.model = State().rendererWorld.GetModelMatrix(renderSubmesh.entity);
        drawConstants.material = renderSubmesh.material;
        drawItems.push_back(VulkanDrawItem{
            renderSubmesh.buffer->GetVertexHandle(),
            renderSubmesh.buffer->GetIndexHandle(),
            renderSubmesh.buffer->GetIndexCount(),
            m_uniformBuffer->GetDescriptorSet(imageIndex, renderSubmesh.materialBindingIndex),
            drawConstants,
            renderSubmesh.doubleSided
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

    // Double-sided materials (e.g. glTF doubleSided=true glass/foliage) get a dedicated
    // no-cull pipeline instead of the default backface-culled one; switch between the two
    // as needed rather than rebinding for every draw item.
    const VulkanPipeline* boundPipeline = nullptr;

    for (const VulkanDrawItem& drawItem : drawItems)
    {
        const VulkanPipeline* requiredPipeline =
            drawItem.doubleSided ? m_graphicsPipelineDoubleSided.get() : m_graphicsPipeline.get();
        if (requiredPipeline != boundPipeline)
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, requiredPipeline->GetHandle());
            boundPipeline = requiredPipeline;
        }

        const VkBuffer vertexBuffers[] = { drawItem.vertexBuffer };
        const VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, drawItem.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            boundPipeline->GetLayout(),
            0,
            1,
            &drawItem.descriptorSet,
            0,
            nullptr
        );
        vkCmdPushConstants(
            commandBuffer,
            boundPipeline->GetLayout(),
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
