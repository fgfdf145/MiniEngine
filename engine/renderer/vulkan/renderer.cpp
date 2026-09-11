#include "renderer.h"

#include "../imgui/imgui_impl_vulkan.h"
#include <engine/editor/renderer_shared_state.h>

#include <engine/logic/editor_world.h>
#include <engine/scene/scene_components.h>
#include <imgui.h>
#include <engine/core/log/log.h>
#include <engine/platform/window/window.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <future>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace me
{

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
                           const LightComponent& light)
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
    texture.pixels = {red, green, blue, alpha};
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
    const std::vector<MaterialTextureSlots>& materialTextureSlots)
{
    std::vector<MaterialTextureBinding> bindings;
    bindings.reserve(materialTextureSlots.size());

    for (const MaterialTextureSlots& slots : materialTextureSlots)
    {
        bindings.push_back(MaterialTextureBinding{
            {textures[slots.baseColor]->GetImageView(), textures[slots.baseColor]->GetSampler()},
            {textures[slots.normal]->GetImageView(), textures[slots.normal]->GetSampler()},
            {textures[slots.metallic]->GetImageView(), textures[slots.metallic]->GetSampler()},
            {textures[slots.roughness]->GetImageView(), textures[slots.roughness]->GetSampler()},
            {textures[slots.occlusion]->GetImageView(), textures[slots.occlusion]->GetSampler()},
            {textures[slots.emissive]->GetImageView(), textures[slots.emissive]->GetSampler()},
            {textures[slots.secondaryBaseColor]->GetImageView(), textures[slots.secondaryBaseColor]->GetSampler()},
            {textures[slots.secondaryNormal]->GetImageView(), textures[slots.secondaryNormal]->GetSampler()},
            {textures[slots.secondaryMetallic]->GetImageView(), textures[slots.secondaryMetallic]->GetSampler()},
            {textures[slots.secondaryRoughness]->GetImageView(), textures[slots.secondaryRoughness]->GetSampler()},
            {textures[slots.secondaryOcclusion]->GetImageView(), textures[slots.secondaryOcclusion]->GetSampler()},
            {textures[slots.secondaryEmissive]->GetImageView(), textures[slots.secondaryEmissive]->GetSampler()},
            {textures[slots.blendMask]->GetImageView(), textures[slots.blendMask]->GetSampler()}});
    }

    return bindings;
}

VkExtent2D ToVkExtent(RenderExtent extent)
{
    return VkExtent2D{
        std::max(extent.width, 1u),
        std::max(extent.height, 1u)};
}

RenderExtent FromVkExtent(VkExtent2D extent)
{
    return RenderExtent{
        extent.width,
        extent.height};
}

// The access and stage masks a target's layout implies. A barrier is described entirely by the
// layouts it moves between, so these two functions are all RecordTransitions needs to turn a
// TargetTransition into a VkImageMemoryBarrier.
VkAccessFlags AccessMaskForLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_ACCESS_SHADER_READ_BIT;
    default:
        return 0;
    }
}

VkPipelineStageFlags StageMaskForLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    case VK_IMAGE_LAYOUT_UNDEFINED:
        return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    default:
        return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

void LogVulkanRuntimeInfo()
{
    uint32_t apiVersion = 0;
    CheckVulkan(vkEnumerateInstanceVersion(&apiVersion), "Failed to query Vulkan runtime version");
    LOG_INFO(
        "Vulkan runtime API version: {}.{}.{}",
        VK_API_VERSION_MAJOR(apiVersion),
        VK_API_VERSION_MINOR(apiVersion),
        VK_API_VERSION_PATCH(apiVersion));
}
}

VulkanRenderer::VulkanRenderer(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    std::optional<std::string> startupModelPath)
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
        m_device->GetGraphicsQueue());
    CreateDeviceResources();
    CreateSwapchainResources();
    UploadSceneResources();
}

VulkanRenderer::~VulkanRenderer()
{
    if (m_device)
    {
        vkDeviceWaitIdle(m_device->GetHandle());
    }

    DestroyDescriptorResources();
    m_graphicsPipelines.reset();
    DestroySwapchainResources();
    m_tonemapPass.reset();
    m_forwardPass.reset();
    m_sceneTargets.reset();
    m_imguiLayer.reset();
    m_textures.clear();
    m_renderSubmeshes.clear();
    DestroyDeviceResources();
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

    EditorWorld().FlushDirtyTransforms();

    SyncSceneTargets();

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

    UpdateViewportMatrices(FromVkExtent(m_sceneTargets->GetExtent()));

    m_imguiLayer->BeginFrame();
    State().editorUi.BeginFrame(GetWindow().GetSDLWindow(), State().engineSettings);
    // ImGui samples the tone mapped image, which is the LDR target and so is indexed by
    // swapchain image: its texture binding is handed out here, before the command buffer that
    // writes it is recorded.
    const EditorUiFrameResult uiFrame = DrawEditorUi(
        m_sceneTargets->GetLdrTextureId(imageIndex),
        FromVkExtent(m_sceneTargets->GetExtent()));
    ApplyUiActions(uiFrame);
    EditorWorld().FlushDirtyTransforms();
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

    ScenePassFrameContext frame{};
    frame.imageIndex = imageIndex;
    frame.frameSlot = m_commandContext->GetCurrentFrame();
    frame.extent = m_sceneTargets->GetExtent();
    frame.drawItems = drawItems;
    frame.pipelines = m_graphicsPipelines.get();
    frame.frameDescriptorSet = m_uniformBuffer->GetFrameDescriptorSet(imageIndex);

    m_commandContext->RecordCommandBuffer(imageIndex, [&](VkCommandBuffer commandBuffer)
                                          {
                                              // The tracker holds one layout per target, but a target has one
                                              // image per copy, so what it learned last frame describes a
                                              // different VkImage than this frame touches. Every command buffer
                                              // therefore starts from undefined rather than carry a layout
                                              // across to the wrong image, which is why the reset lives here,
                                              // where the command buffer begins, and covers the ImGui
                                              // declaration below as well as the scene passes.
                                              //
                                              // That costs nothing: AcquireNextImage already waited on this
                                              // frame slot's fence, so the previous use of these images has
                                              // completed, and every target is cleared or fully rewritten
                                              // before it is read, so discarding its contents is what we want
                                              // anyway.
                                              m_layoutTracker.Reset();

                                              RecordScenePasses(commandBuffer, frame);

                                              // ImGui samples the tone mapped image in the editor pass, which is
                                              // not an IScenePass because it writes the swapchain rather than a
                                              // scene target.
                                              static constexpr std::array<RenderTargetId, 1> kImGuiReads = {RenderTargetId::SceneLdr};
                                              RenderPassIo imguiIo{};
                                              imguiIo.reads = kImGuiReads;
                                              RecordTransitions(commandBuffer, imguiIo, frame);

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
        supportDetails);
    m_renderPass = std::make_unique<VulkanRenderPass>(
        m_device->GetHandle(),
        m_swapchain->GetImageFormat(),
        m_swapchain->GetExtent(),
        m_swapchain->GetImageViews());
    m_commandContext = std::make_unique<VulkanCommandContext>(
        m_device->GetHandle(),
        m_device->GetQueueFamilies(),
        m_renderPass->GetFramebuffers().size());
    m_imguiLayer->CreateOrUpdateVulkanResources(m_renderPass->GetHandle(), static_cast<uint32_t>(m_swapchain->GetImageViews().size()));
    if (!State().requestedViewportExtent.IsValid())
    {
        State().requestedViewportExtent = FromVkExtent(m_swapchain->GetExtent());
    }

    // Anything that throws below propagates out of the frame path with m_scenePasses left empty
    // (DestroySwapchainResources cleared it), so no half-built target set is ever recordable.
    // SceneRenderTargets does not unwind the images it already created when its constructor
    // throws, so recovering in place here is not possible; failing loudly is the whole handling.
    const VkExtent2D viewportExtent = ToVkExtent(State().requestedViewportExtent);
    const uint32_t swapchainImageCount = static_cast<uint32_t>(m_swapchain->GetImageViews().size());
    // Rebuild re-creates the images at a new size and image count but never re-runs format
    // selection, so it can only carry the target set across a swapchain recreate while the LDR
    // target's format still matches the swapchain's. A surface format change is rare but real,
    // and it has to reach the LDR target: the tone mapping pass builds its render pass on that
    // format and ImGui samples the image, so a stale one would be a wrong-format viewport. Only
    // a fresh SceneRenderTargets re-runs the selection, so that case is reconstructed outright.
    const bool ldrFormatMatchesSwapchain =
        m_sceneTargets != nullptr &&
        m_sceneTargets->GetFormat(RenderTargetId::SceneLdr) == m_swapchain->GetImageFormat();
    if (ldrFormatMatchesSwapchain)
    {
        m_sceneTargets->Rebuild(viewportExtent, swapchainImageCount);
    }
    else
    {
        m_sceneTargets = std::make_unique<SceneRenderTargets>(
            m_device->GetPhysicalDevice(),
            m_device->GetHandle(),
            m_swapchain->GetImageFormat(),
            viewportExtent,
            swapchainImageCount);
    }

    // The forward pass renders into the HDR target, whose format is fixed and independent of the
    // swapchain image format, so its render pass never needs recreating here — not even when the
    // swapchain format moved — and the material pipelines built against it stay valid across
    // every swapchain recreate. Only its framebuffers follow the rebuilt views.
    if (m_forwardPass)
    {
        m_forwardPass->OnTargetsRebuilt(*m_sceneTargets);
    }
    else
    {
        m_forwardPass = std::make_unique<VulkanForwardPass>(m_device->GetHandle(), *m_sceneTargets);
    }

    // The tone mapping pass owns the only render pass in the frame that references the LDR
    // format, so unlike the forward pass it cannot survive a swapchain format change: when the
    // format moved it is replaced rather than pointed at the new views.
    if (m_tonemapPass && ldrFormatMatchesSwapchain)
    {
        m_tonemapPass->OnTargetsRebuilt(*m_sceneTargets);
    }
    else
    {
        m_tonemapPass = std::make_unique<VulkanTonemapPass>(
            m_device->GetHandle(),
            m_pipelineCache,
            *m_sceneTargets);
    }

    m_layoutTracker.Reset();
    m_scenePasses = {m_forwardPass.get(), m_tonemapPass.get()};
}

void VulkanRenderer::DestroySwapchainResources()
{
    // Keep the pass objects alive (and with them the render pass the pipelines were built
    // against), but drop the target images: the LDR copies are sized by the swapchain image
    // count, and every ImGui texture binding must be released before ImGui's descriptor pool goes
    // away. Nothing is recordable until CreateSwapchainResources repopulates the pass list, and a
    // released image is undefined again, so the tracker goes back to square one with it.
    m_scenePasses.clear();
    if (m_sceneTargets)
    {
        m_sceneTargets->ReleaseImages();
    }
    m_layoutTracker.Reset();
    if (m_imguiLayer)
    {
        m_imguiLayer->DestroyVulkanResources();
    }
    m_commandContext.reset();
    m_renderPass.reset();
    m_swapchain.reset();
}

void VulkanRenderer::CreateDeviceResources()
{
    // All three live as long as the logical device. The frame and material descriptor set
    // layouts are fixed by the shader, so hoisting them out of VulkanUniformBuffer lets a scene
    // reload rebuild descriptor sets without invalidating the pipelines. The pipeline cache
    // outliving every VulkanPipelineSet is what lets a rebuild reuse the driver's earlier shader
    // compilation.
    m_frameSetLayout = std::make_unique<VulkanFrameDescriptorSetLayout>(m_device->GetHandle());
    m_materialSetLayout = std::make_unique<VulkanMaterialDescriptorSetLayout>(m_device->GetHandle());

    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    CheckVulkan(
        vkCreatePipelineCache(m_device->GetHandle(), &cacheInfo, nullptr, &m_pipelineCache),
        "Failed to create pipeline cache");
}

void VulkanRenderer::DestroyDeviceResources()
{
    if (m_pipelineCache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(m_device->GetHandle(), m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
    }
    m_materialSetLayout.reset();
    m_frameSetLayout.reset();
}

void VulkanRenderer::EnsureGraphicsPipelines()
{
    if (m_graphicsPipelines)
    {
        return;
    }

    m_graphicsPipelines = std::make_unique<VulkanPipelineSet>(
        m_device->GetHandle(),
        m_pipelineCache,
        m_forwardPass->GetRenderPass(),
        m_frameSetLayout->GetHandle(),
        m_materialSetLayout->GetHandle());
}

void VulkanRenderer::CreateDescriptorResources()
{
    if (m_textures.empty())
    {
        throw std::runtime_error("Cannot create descriptor resources without at least one texture");
    }
    if (m_materialTextureSlots.empty())
    {
        throw std::runtime_error("Cannot create descriptor resources without at least one material texture binding");
    }

    m_uniformBuffer = std::make_unique<VulkanUniformBuffer>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        static_cast<uint32_t>(m_swapchain->GetImageViews().size()),
        m_frameSetLayout->GetHandle(),
        m_materialSetLayout->GetHandle(),
        BuildMaterialTextureBindings(m_textures, m_materialTextureSlots));
    EnsureGraphicsPipelines();
}

void VulkanRenderer::DestroyDescriptorResources()
{
    m_uniformBuffer.reset();
    // The pool holds VulkanTexture objects that reference VkImage/VkSampler; clear it whenever the
    // descriptor sets are torn down so nothing outlives the logical device. m_textureCacheKeys must
    // NOT be cleared here: it stays index-paired with m_textures (which survives this teardown),
    // and wiping it makes the next UploadSceneResources destroy every live texture instead of
    // pooling it — while in-flight frames may still be sampling them.
    m_texturePool.clear();
}

void VulkanRenderer::RecreateSwapchain()
{
    if (!HasDrawableArea())
    {
        return;
    }

    vkDeviceWaitIdle(m_device->GetHandle());
    DestroyDescriptorResources();
    DestroySwapchainResources();
    CreateSwapchainResources();
    CreateDescriptorResources();
}

void VulkanRenderer::SyncSceneTargets()
{
    if (!m_swapchain || !m_sceneTargets)
    {
        return;
    }

    if (!State().requestedViewportExtent.IsValid())
    {
        State().requestedViewportExtent = FromVkExtent(m_sceneTargets->GetExtent());
    }

    if (m_sceneTargets->MatchesExtent(ToVkExtent(State().requestedViewportExtent)))
    {
        return;
    }

    // Only the target images and the per-image resources built from them depend on the extent.
    // Both passes' render passes and the targets' sampler survive, and the pipelines use dynamic
    // viewport/scissor state, so neither they nor the uniform buffer have to be rebuilt while the
    // user drags the viewport edge. The images are new, so the tracker goes back to undefined.
    vkDeviceWaitIdle(m_device->GetHandle());
    m_sceneTargets->Rebuild(
        ToVkExtent(State().requestedViewportExtent),
        static_cast<uint32_t>(m_swapchain->GetImageViews().size()));
    m_forwardPass->OnTargetsRebuilt(*m_sceneTargets);
    m_tonemapPass->OnTargetsRebuilt(*m_sceneTargets);
    m_layoutTracker.Reset();
    LOG_INFO(
        "Scene render targets resized to {}x{}",
        m_sceneTargets->GetExtent().width,
        m_sceneTargets->GetExtent().height);
}

void VulkanRenderer::UploadSceneResources()
{
    // Move live textures into the pool so they can be reused without re-uploading.
    // This prevents 2x peak VRAM usage when rebuilding an unchanged texture set.
    // Anything that can't be pooled is parked in retiredTextures instead of being destroyed
    // here: the GPU may still be sampling it through the old descriptor sets, so its
    // destruction must wait until after ApplyRenderContent has waited for in-flight frames.
    std::vector<std::unique_ptr<VulkanTexture>> retiredTextures;
    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        if (!m_textures[i])
        {
            continue;
        }
        if (i < m_textureCacheKeys.size() && !m_textureCacheKeys[i].empty() &&
            m_texturePool.emplace(m_textureCacheKeys[i], std::move(m_textures[i])).second)
        {
            continue;
        }
        retiredTextures.push_back(std::move(m_textures[i]));
    }
    m_textures.clear();
    m_textureCacheKeys.clear();

    std::vector<std::unique_ptr<VulkanTexture>> newTextures;
    std::vector<std::string> newCacheKeys;
    std::vector<MaterialTextureSlots> newMaterialTextureSlots;
    std::vector<RenderSubmesh> newRenderSubmeshes;
    std::unordered_map<std::string, uint32_t> keyToIndex;

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
        m_device->GetGraphicsQueue());
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
                data, uploadBatch, fmt));
            flushUploadBatchIfNeeded();
        }
        newCacheKeys.push_back(key);
        keyToIndex.emplace(key, idx);
        return idx;
    };

    auto loadTextureIndex = [&](const std::string& texturePath, VulkanTextureFormat textureFormat, uint32_t fallbackIndex) -> uint32_t
    {
        if (texturePath.empty())
            return fallbackIndex;

        const std::string key = BuildTextureCacheKey(texturePath, textureFormat);
        if (auto it = keyToIndex.find(key); it != keyToIndex.end())
            return it->second;

        // Pool hit: reuse existing GPU texture 鈥?no disk I/O, no upload, no extra VRAM.
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
                texturePath, uploadBatch, textureFormat));
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

    const uint32_t defaultBaseColorIndex = acquireDefault("__default_base_color__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::SrgbColor);
    const uint32_t defaultNormalIndex = acquireDefault("__default_normal__", CreateFlatNormalTexture(), VulkanTextureFormat::LinearData);
    const uint32_t defaultMetallicIndex = acquireDefault("__default_metallic__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
    const uint32_t defaultRoughnessIndex = acquireDefault("__default_roughness__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
    const uint32_t defaultOcclusionIndex = acquireDefault("__default_occlusion__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
    const uint32_t defaultEmissiveIndex = acquireDefault("__default_emissive__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::SrgbColor);
    const uint32_t defaultBlendMaskIndex = acquireDefault("__default_blend_mask__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);

    const uint32_t defaultMaterialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
    newMaterialTextureSlots.push_back(MaterialTextureSlots{
        defaultBaseColorIndex, defaultNormalIndex, defaultMetallicIndex, defaultRoughnessIndex,
        defaultOcclusionIndex, defaultEmissiveIndex,
        defaultBaseColorIndex, defaultNormalIndex, defaultMetallicIndex, defaultRoughnessIndex,
        defaultOcclusionIndex, defaultEmissiveIndex, defaultBlendMaskIndex});

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
            pendingLoads.push_back(PendingTextureLoad{key, path, format, {}, false, {}});
        };

        for (const CpuRenderSubmesh& cpuRenderSubmesh : State().rendererWorld.GetRenderSubmeshes())
        {
            if (!cpuRenderSubmesh.hasTexCoords)
            {
                continue;
            }
            const MaterialTexturePaths& textures = cpuRenderSubmesh.textures;
            considerPath(textures.baseColor, VulkanTextureFormat::SrgbColor);
            considerPath(textures.normal, VulkanTextureFormat::LinearData);
            considerPath(textures.metallic, VulkanTextureFormat::LinearData);
            considerPath(textures.roughness, VulkanTextureFormat::LinearData);
            considerPath(textures.occlusion, VulkanTextureFormat::LinearData);
            considerPath(textures.emissive, VulkanTextureFormat::SrgbColor);
            considerPath(textures.secondaryBaseColor, VulkanTextureFormat::SrgbColor);
            considerPath(textures.secondaryNormal, VulkanTextureFormat::LinearData);
            considerPath(textures.secondaryMetallic, VulkanTextureFormat::LinearData);
            considerPath(textures.secondaryRoughness, VulkanTextureFormat::LinearData);
            considerPath(textures.secondaryOcclusion, VulkanTextureFormat::LinearData);
            considerPath(textures.secondaryEmissive, VulkanTextureFormat::SrgbColor);
            considerPath(textures.blendMask, VulkanTextureFormat::LinearData);
        }

        // Decode in bounded chunks rather than all at once, so we don't hold dozens of huge
        // decoded RGBA8 buffers in host memory simultaneously (a 40 MB source PNG can decode to
        // 100+ MB of raw pixels).
        const size_t chunkSize = std::max<size_t>(8, static_cast<size_t>(std::thread::hardware_concurrency()) * 2);
        for (size_t chunkStart = 0; chunkStart < pendingLoads.size(); chunkStart += chunkSize)
        {
            const size_t chunkEnd = std::min(chunkStart + chunkSize, pendingLoads.size());

            // std::async instead of std::execution::par: libc++ on macOS has no
            // parallel algorithm support, and the chunk size already bounds the
            // number of concurrent decode threads.
            std::vector<std::future<void>> decodeTasks;
            decodeTasks.reserve(chunkEnd - chunkStart);
            for (size_t i = chunkStart; i < chunkEnd; ++i)
            {
                decodeTasks.push_back(std::async(std::launch::async, [&pending = pendingLoads[i]]()
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
                                                 }));
            }
            for (std::future<void>& decodeTask : decodeTasks)
            {
                decodeTask.wait();
            }

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
                        pending.decoded, uploadBatch, pending.format));
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
        renderSubmesh.entity = cpuRenderSubmesh.entity;
        renderSubmesh.buffer = std::make_unique<VulkanBuffer>(
            m_device->GetPhysicalDevice(), m_device->GetHandle(),
            cpuRenderSubmesh.mesh, uploadBatch);
        flushUploadBatchIfNeeded();
        renderSubmesh.material = cpuRenderSubmesh.material;
        renderSubmesh.doubleSided = cpuRenderSubmesh.doubleSided;
        renderSubmesh.alphaMode = cpuRenderSubmesh.alphaMode;
        renderSubmesh.localBoundsCenter = cpuRenderSubmesh.localBoundsCenter;
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
        slots.secondaryBaseColor = loadTextureIndex(cpuRenderSubmesh.textures.secondaryBaseColor, VulkanTextureFormat::SrgbColor, slots.baseColor);
        slots.secondaryNormal = loadTextureIndex(cpuRenderSubmesh.textures.secondaryNormal, VulkanTextureFormat::LinearData, slots.normal);
        slots.secondaryMetallic = loadTextureIndex(cpuRenderSubmesh.textures.secondaryMetallic, VulkanTextureFormat::LinearData, slots.metallic);
        slots.secondaryRoughness = loadTextureIndex(cpuRenderSubmesh.textures.secondaryRoughness, VulkanTextureFormat::LinearData, slots.roughness);
        slots.secondaryOcclusion = loadTextureIndex(cpuRenderSubmesh.textures.secondaryOcclusion, VulkanTextureFormat::LinearData, slots.occlusion);
        slots.secondaryEmissive = loadTextureIndex(cpuRenderSubmesh.textures.secondaryEmissive, VulkanTextureFormat::SrgbColor, slots.emissive);
        slots.blendMask = loadTextureIndex(cpuRenderSubmesh.textures.blendMask, VulkanTextureFormat::LinearData, defaultBlendMaskIndex);

        renderSubmesh.materialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
        newMaterialTextureSlots.push_back(slots);
        newRenderSubmeshes.push_back(std::move(renderSubmesh));
    }
    uploadBatch.Flush();
    LOG_INFO("Uploaded {} submesh buffers and {} textures", newRenderSubmeshes.size(), newTextures.size());

    ApplyRenderContent(std::move(newTextures), std::move(newMaterialTextureSlots), std::move(newRenderSubmeshes));

    // Textures remaining in the pool (and the retired ones) are no longer referenced; destroy
    // them only now that ApplyRenderContent has waited for the in-flight frames and destroyed
    // the old descriptor sets. Clearing earlier destroys samplers/image views the GPU may still
    // be reading through the old descriptor sets (caught by validation as
    // vkDestroySampler-while-in-use).
    m_texturePool.clear();
    retiredTextures.clear();

    // Track the cache keys for the textures now in m_textures, so the next upload can pool them.
    m_textureCacheKeys = std::move(newCacheKeys);
}

void VulkanRenderer::ApplyRenderContent(
    std::vector<std::unique_ptr<VulkanTexture>> newTextures,
    std::vector<MaterialTextureSlots> newMaterialTextureSlots,
    std::vector<RenderSubmesh> newRenderSubmeshes)
{
    std::unique_ptr<VulkanUniformBuffer> newUniformBuffer;

    if (m_swapchain && m_renderPass && m_forwardPass && !newTextures.empty() && !newMaterialTextureSlots.empty())
    {
        // Only the descriptor sets are rebuilt for a new texture set. The pipelines are built
        // against the renderer's fixed frame and material set layouts and the forward pass's
        // render pass, none of which a content reload touches, so they are left alone.
        newUniformBuffer = std::make_unique<VulkanUniformBuffer>(
            m_device->GetPhysicalDevice(),
            m_device->GetHandle(),
            static_cast<uint32_t>(m_swapchain->GetImageViews().size()),
            m_frameSetLayout->GetHandle(),
            m_materialSetLayout->GetHandle(),
            BuildMaterialTextureBindings(newTextures, newMaterialTextureSlots));
        // Wait only for our in-flight render frames to finish before destroying old resources.
        // vkWaitForFences is more targeted than vkDeviceWaitIdle: it doesn't stall the
        // present or transfer queues, and the new UBO above is built while the GPU may still
        // be executing the previous frame (overlapping CPU and GPU work).
        m_commandContext->WaitForAllFrames();
        m_uniformBuffer = std::move(newUniformBuffer);
        EnsureGraphicsPipelines();
    }

    m_textures = std::move(newTextures);
    m_materialTextureSlots = std::move(newMaterialTextureSlots);
    m_renderSubmeshes = std::move(newRenderSubmeshes);
}

std::vector<VulkanDrawItem> VulkanRenderer::BuildDrawItems(uint32_t imageIndex) const
{
    std::vector<VulkanDrawItem> unsorted;
    std::vector<MaterialDrawSortKey> sortKeys;
    unsorted.reserve(m_renderSubmeshes.size());
    sortKeys.reserve(m_renderSubmeshes.size());

    for (const RenderSubmesh& renderSubmesh : m_renderSubmeshes)
    {
        ObjectPushConstants drawConstants{};
        drawConstants.model = State().rendererWorld.GetModelMatrix(renderSubmesh.entity);
        drawConstants.material = renderSubmesh.material;
        const MaterialPipelineKey pipelineKey{
            renderSubmesh.alphaMode,
            renderSubmesh.doubleSided};
        const glm::vec4 viewCenter =
            State().viewportMatrices.view *
            drawConstants.model *
            glm::vec4(renderSubmesh.localBoundsCenter, 1.0f);
        sortKeys.push_back({pipelineKey, -viewCenter.z});
        unsorted.push_back(VulkanDrawItem{
            renderSubmesh.buffer->GetVertexHandle(),
            renderSubmesh.buffer->GetIndexHandle(),
            renderSubmesh.buffer->GetIndexCount(),
            m_uniformBuffer->GetDescriptorSet(imageIndex, renderSubmesh.materialBindingIndex),
            drawConstants,
            pipelineKey});
    }

    std::vector<VulkanDrawItem> ordered;
    ordered.reserve(unsorted.size());
    for (size_t index : BuildMaterialDrawOrder(sortKeys))
    {
        ordered.push_back(std::move(unsorted[index]));
    }
    return ordered;
}

void VulkanRenderer::RecordTransitions(
    VkCommandBuffer commandBuffer,
    const RenderPassIo& io,
    const ScenePassFrameContext& frame)
{
    for (const TargetTransition& transition : m_layoutTracker.Transition(io))
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = transition.oldLayout;
        barrier.newLayout = transition.newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_sceneTargets->GetImage(
            transition.target,
            m_sceneTargets->ResolveIndex(transition.target, frame.imageIndex, frame.frameSlot));
        barrier.subresourceRange.aspectMask = m_sceneTargets->GetAspect(transition.target);
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = AccessMaskForLayout(transition.oldLayout);
        barrier.dstAccessMask = AccessMaskForLayout(transition.newLayout);

        vkCmdPipelineBarrier(
            commandBuffer,
            StageMaskForLayout(transition.oldLayout),
            StageMaskForLayout(transition.newLayout),
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
    }
}

void VulkanRenderer::RecordScenePasses(VkCommandBuffer commandBuffer, const ScenePassFrameContext& frame)
{
    // The layout tracker is reset by the caller, at the head of the command buffer it describes.
    for (IScenePass* pass : m_scenePasses)
    {
        RecordTransitions(commandBuffer, pass->Io(), frame);
        pass->Record(commandBuffer, *m_sceneTargets, frame);
    }
}

void VulkanRenderer::RecordEditorLayer(VkCommandBuffer commandBuffer, uint32_t imageIndex) const
{
    // Single color attachment: the ImGui pass has no depth buffer (see VulkanRenderPass).
    VkClearValue clearValue{};
    clearValue.color = {{0.04f, 0.05f, 0.08f, 1.0f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass->GetHandle();
    renderPassInfo.framebuffer = m_renderPass->GetFramebuffers()[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = m_swapchain->GetExtent();
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    vkCmdEndRenderPass(commandBuffer);
}
}
