#include "renderer.h"

#include "../imgui/imgui_impl_vulkan.h"
#include <engine/editor/renderer_shared_state.h>
#include <engine/renderer/scene_lighting.h>

#include <engine/logic/editor_world.h>
#include <engine/scene/scene_components.h>
#include <imgui.h>
#include <engine/asset/compressed_texture_cache.h>
#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/platform/window/window.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace me
{

namespace
{
// Per cascade. Four 2048 x 2048 32-bit layers are 64 MiB.
constexpr uint32_t kShadowMapResolution = 2048;

// A transform's Euler rotation (XYZ order, same as BuildTransformMatrix). Directional and spot
// lights shine along local -Y. An area light is the rectangle DrawLightAreaGizmo draws: it lies in
// the local XY plane, width along +X and height along Y, and emits along local -Z, the gizmo's
// normal arrow.
glm::mat3 BuildLightRotation(const TransformComponent& transform)
{
    glm::mat4 rotMat(1.0f);
    rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
    rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
    rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::mat3(rotMat);
}

// Every light in the scene, in scene order, as the shader wants it and as the light selection
// ranks it. The two vectors are parallel.
struct CollectedSceneLights
{
    std::vector<GpuLightData> gpuLights;
    std::vector<SceneLightCandidate> candidates;
};

CollectedSceneLights CollectSceneLights(const IEditorWorld& world)
{
    CollectedSceneLights collected;
    world.ForEachLight([&](
                           entt::entity,
                           const TagComponent&,
                           const TransformComponent& transform,
                           const LightComponent& light)
                       {
                           GpuLightData gpu{};
                           gpu.positionAndRange = glm::vec4(transform.translation, light.range);
                           gpu.colorAndIntensity = glm::vec4(light.color, light.intensity);

                           const glm::mat3 rotation = BuildLightRotation(transform);
                           const glm::vec3 localDirection = light.type == LightType::Area
                                                                ? glm::vec3(0.0f, 0.0f, -1.0f)
                                                                : glm::vec3(0.0f, -1.0f, 0.0f);
                           const glm::vec3 direction = glm::normalize(rotation * localDirection);
                           gpu.directionAndType = glm::vec4(direction, static_cast<float>(light.type));
                           gpu.areaRightAxis = glm::vec4(glm::normalize(rotation * glm::vec3(1.0f, 0.0f, 0.0f)), 0.0f);

                           const float innerCos = std::cos(glm::radians(light.spotInnerAngleDegrees));
                           const float outerCos = std::cos(glm::radians(light.spotOuterAngleDegrees));
                           // The area gizmo draws the rectangle through the whole transform, scale
                           // included, so the lit rectangle is scaled the same way to match it. The
                           // clamp is BuildTransformMatrix's.
                           const glm::vec3 scale = glm::max(transform.scale, WorldUnits::kMinimumScale3);
                           gpu.spotAndArea = glm::vec4(
                               innerCos,
                               outerCos,
                               light.areaSize.x * scale.x,
                               light.areaSize.y * scale.y);

                           SceneLightCandidate candidate{};
                           candidate.type = light.type;
                           candidate.position = transform.translation;
                           candidate.color = light.color;
                           candidate.intensity = light.intensity;

                           collected.gpuLights.push_back(gpu);
                           collected.candidates.push_back(candidate);
                       });
    return collected;
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

// Live textures are reused by this key. The usage is part of it because the same file can be two
// textures: a normal map uploaded as BC5 is not the same image as that file uploaded as BC7 data.
std::string BuildTextureCacheKey(const std::string& path, TextureUsage usage)
{
    switch (usage)
    {
    case TextureUsage::Color:
        return path + "|color";
    case TextureUsage::Normal:
        return path + "|normal";
    case TextureUsage::Data:
        return path + "|data";
    }
    throw std::runtime_error("Unknown texture usage");
}

VulkanTextureFormat ToVulkanTextureFormat(TextureUsage usage)
{
    return usage == TextureUsage::Color ? VulkanTextureFormat::SrgbColor : VulkanTextureFormat::LinearData;
}

// A material texture file ready to upload: block-compressed when the device samples BC formats,
// RGBA8 otherwise.
struct PreparedTexture
{
    std::optional<CompressedTexture> compressed;
    TextureData rgba;
    bool fromCache = false;
    double compressSeconds = 0.0;
};

// The CPU half of loading one texture file; safe to run on any thread. A texture that cannot be
// compressed or cached is uploaded as RGBA8 instead, on its own; one that cannot be decoded at all
// throws, and the caller falls back to the slot's default texture as before.
PreparedTexture PrepareTexture(const std::string& path, TextureUsage usage, bool compress)
{
    PreparedTexture prepared{};
    if (compress)
    {
        try
        {
            const auto start = std::chrono::steady_clock::now();
            CompressedTextureLoad load = LoadOrCompressTexture(path, usage, EnginePaths::CacheRoot() / "textures");
            prepared.fromCache = load.cacheHit;
            prepared.compressSeconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            prepared.compressed = std::move(load.texture);
            return prepared;
        }
        catch (const std::exception& error)
        {
            LOG_ERROR("Could not compress texture '{}', uploading it uncompressed: {}", path, error.what());
        }
    }
    prepared.rgba = TextureLoader::LoadRGBA8(path);
    return prepared;
}

// True for the one failure a content upload recovers from: running out of memory. Everything else
// (a lost device, a driver bug) keeps propagating.
bool IsOutOfMemoryError(const std::exception& error)
{
    const VulkanError* vulkanError = dynamic_cast<const VulkanError*>(&error);
    return vulkanError != nullptr && vulkanError->IsOutOfMemory();
}

std::vector<const VulkanTexture*> ViewTextures(const std::vector<std::unique_ptr<VulkanTexture>>& textures)
{
    std::vector<const VulkanTexture*> views;
    views.reserve(textures.size());
    for (const std::unique_ptr<VulkanTexture>& texture : textures)
    {
        views.push_back(texture.get());
    }
    return views;
}

std::vector<MaterialTextureBinding> BuildMaterialTextureBindings(
    std::span<const VulkanTexture* const> textures,
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
    case VK_IMAGE_LAYOUT_UNDEFINED:
        return 0;
    default:
        // An unhandled layout would otherwise yield an access mask of 0, producing a barrier that
        // changes the layout with no memory dependency at all — the one failure mode this
        // abstraction exists to prevent, and one validation layers do not flag. Throwing rather
        // than asserting matches RenderTargetLayoutTracker, so the rule also holds in Release.
        throw std::runtime_error(
            "AccessMaskForLayout has no access mask for image layout " +
            std::to_string(static_cast<int32_t>(layout)));
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
        // Sampled by fragment shaders (tone mapping, ImGui) and by the exposure histogram's
        // compute shader, so a transition into or out of this layout has to cover both.
        return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
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
    m_forwardPipelines.reset();
    m_geometryPipelines.reset();
    DestroySwapchainResources();
    m_scenePasses.clear();
    m_exposurePass = nullptr;
    m_gbufferDescriptors.reset();
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
        UploadSceneResourcesOrKeepPrevious();
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

    UpdateAutoExposure(m_commandContext->GetCurrentFrame());
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
        UploadSceneResourcesOrKeepPrevious();
        State().renderablesDirty = false;
    }
    ImGui::Render();

    const CollectedSceneLights sceneLights =
        State().editorWorld ? CollectSceneLights(*State().editorWorld) : CollectedSceneLights{};
    const SceneLightSelection lightSelection =
        SelectSceneLights(sceneLights.candidates, State().camera.position, kMaxSceneLights);
    ReportDroppedLights(lightSelection.droppedCount);
    std::vector<GpuLightData> selectedLights;
    selectedLights.reserve(lightSelection.selected.size());
    for (uint32_t index : lightSelection.selected)
    {
        selectedLights.push_back(sceneLights.gpuLights[index]);
    }

    ShadowUniformData shadowData{};
    std::optional<ShadowCascades> shadowCascades;
    const int32_t shadowLightIndex = SelectShadowCasterLight(sceneLights.candidates, lightSelection);
    if (shadowLightIndex >= 0)
    {
        const VkExtent2D extent = m_sceneTargets->GetExtent();
        ShadowCameraInput shadowCamera{};
        shadowCamera.view = State().viewportMatrices.view;
        shadowCamera.verticalFovRadians = glm::radians(State().camera.fovDegrees);
        shadowCamera.aspect = static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u));
        shadowCamera.nearPlane = State().camera.nearPlane;
        shadowCamera.farPlane = State().camera.farPlane;
        ShadowCascadeSettings shadowSettings{};
        shadowSettings.resolution = m_shadowPass->GetResolution();
        shadowCascades = BuildShadowCascades(
            shadowCamera,
            glm::vec3(selectedLights[shadowLightIndex].directionAndType),
            shadowSettings);

        for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
        {
            shadowData.cascadeViewProjection[cascade] = (*shadowCascades)[cascade].viewProjection;
            shadowData.cascadeSplits[cascade] = (*shadowCascades)[cascade].splitFar;
            shadowData.cascadeTexelSizes[cascade] = (*shadowCascades)[cascade].texelWorldSize;
        }
        shadowData.params = glm::vec4(
            static_cast<float>(shadowLightIndex),
            1.0f / static_cast<float>(m_shadowPass->GetResolution()),
            0.0f,
            0.0f);
    }

    std::vector<glm::mat4> models;
    std::vector<MotionKey> motionKeys;
    models.reserve(m_renderSubmeshes.size());
    motionKeys.reserve(m_renderSubmeshes.size());
    for (const RenderSubmesh& renderSubmesh : m_renderSubmeshes)
    {
        models.push_back(State().rendererWorld.GetModelMatrix(renderSubmesh.entity));
        motionKeys.push_back(renderSubmesh.motionKey);
    }
    const glm::mat4 viewProjection = State().viewportMatrices.renderProjection * State().viewportMatrices.view;
    const MotionFrame motion = m_motionHistory.Advance(viewProjection, motionKeys, models);

    m_uniformBuffer->Update(
        imageIndex,
        State().viewportMatrices,
        State().camera.position,
        lightSelection.ambientLuminance,
        selectedLights,
        shadowData,
        motion.previousViewProjection,
        motion.previousModels);
    const std::vector<VulkanDrawItem> drawItems = BuildDrawItems(imageIndex, models);
    const std::vector<ShadowDrawItem> shadowDrawItems =
        shadowCascades.has_value() ? BuildShadowDrawItems(imageIndex) : std::vector<ShadowDrawItem>{};

    ScenePassFrameContext frame{};
    frame.imageIndex = imageIndex;
    frame.frameSlot = m_commandContext->GetCurrentFrame();
    frame.extent = m_sceneTargets->GetExtent();
    frame.drawItems = drawItems;
    frame.blendDrawItemBegin = static_cast<size_t>(
        std::partition_point(
            drawItems.begin(),
            drawItems.end(),
            [](const VulkanDrawItem& item)
            {
                return item.pipelineKey.alphaMode != MaterialAlphaMode::Blend;
            }) -
        drawItems.begin());
    frame.forwardPipelines = m_forwardPipelines.get();
    frame.geometryPipelines = m_geometryPipelines.get();
    frame.frameDescriptorSet = m_uniformBuffer->GetFrameDescriptorSet(imageIndex);
    frame.gbufferDescriptorSet = m_gbufferDescriptors->GetSet(*m_sceneTargets, imageIndex, frame.frameSlot);
    // The order and the forward filter both derive from this one switch, here, so they cannot
    // disagree. The forward-only order never runs the geometry pass and leaves the G-buffer
    // undefined, so its debug views are forced off rather than trusted to the UI's disabled state.
    const RenderDebugSettings renderDebug = State().renderDebug;
    const std::span<const ScenePassId> passOrder = BuildScenePassOrder(renderDebug.forwardOnly);
    frame.forwardFilter = renderDebug.forwardOnly ? ForwardDrawFilter::All : ForwardDrawFilter::BlendOnly;
    frame.gbufferView = renderDebug.forwardOnly ? GBufferDebugView::Off : renderDebug.gbufferView;
    frame.exposure = State().camera.GetExposure();

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

                                              // Ahead of the scene passes, whose material pass samples it. It
                                              // orders itself through its render pass dependencies and never
                                              // goes through the layout tracker (see VulkanShadowPass).
                                              m_shadowPass->Record(
                                                  commandBuffer,
                                                  shadowDrawItems,
                                                  shadowCascades.has_value() ? &*shadowCascades : nullptr);

                                              RecordScenePasses(commandBuffer, frame, passOrder);

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

    m_layoutTracker.Reset();
    m_motionHistory.Reset();
    CreateScenePasses();
}

void VulkanRenderer::DestroySwapchainResources()
{
    // Destroying the passes takes their render passes with them, so the pipelines built against
    // them go too; CreateScenePasses rebuilds both together. The target images are released
    // rather than destroyed: the LDR copies are sized by the swapchain image count, and every
    // ImGui texture binding must be released before ImGui's descriptor pool goes away. Nothing is
    // recordable until CreateSwapchainResources repopulates the pass list, and a released image
    // is undefined again, so the tracker goes back to square one with it.
    m_scenePasses.clear();
    m_exposurePass = nullptr;
    m_forwardPipelines.reset();
    m_geometryPipelines.reset();
    m_gbufferDescriptors.reset();
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

    m_shadowPass = std::make_unique<VulkanShadowPass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        m_materialSetLayout->GetHandle(),
        kShadowMapResolution);
}

void VulkanRenderer::DestroyDeviceResources()
{
    // Its pipelines were built against the material set layout released below.
    m_shadowPass.reset();
    if (m_pipelineCache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(m_device->GetHandle(), m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
    }
    m_materialSetLayout.reset();
    m_frameSetLayout.reset();
}

void VulkanRenderer::CreateScenePasses()
{
    // Passes are built fresh here rather than carried across a swapchain recreate. The tone
    // mapping pass could never survive a swapchain format change anyway, and rebuilding the
    // material pipelines alongside them costs almost nothing: m_pipelineCache outlives every
    // pipeline set, so the driver reuses its earlier shader compilation. What that buys is the
    // disappearance of every construct-or-rebuild branch, and with it the window in which the
    // pass list held passes whose framebuffers referenced destroyed views.
    //
    // A viewport resize does NOT come through here. That path rebuilds only the images and tells
    // every owned pass to follow them; see SyncSceneTargets.
    m_scenePasses.clear();
    m_exposurePass = nullptr;
    m_forwardPipelines.reset();
    m_geometryPipelines.reset();
    m_gbufferDescriptors = std::make_unique<VulkanGBufferDescriptors>(m_device->GetHandle(), *m_sceneTargets);

    auto geometryPass = std::make_unique<VulkanGeometryPass>(m_device->GetHandle(), *m_sceneTargets);
    auto forwardPass = std::make_unique<VulkanForwardPass>(m_device->GetHandle(), *m_sceneTargets);

    MaterialPipelineSetConfig geometryConfig{};
    geometryConfig.fragmentShader = "gbuffer.frag.spv";
    geometryConfig.colorAttachmentCount = VulkanGeometryPass::kColorAttachmentCount;
    geometryConfig.writeAlpha = true;
    geometryConfig.allowBlending = false;

    // Both sets are built here, while the typed pass pointers are in hand: the pipelines depend on
    // nothing but these render passes and the two device lifetime set layouts.
    m_geometryPipelines = std::make_unique<VulkanPipelineSet>(
        m_device->GetHandle(),
        m_pipelineCache,
        geometryPass->GetRenderPass(),
        m_frameSetLayout->GetHandle(),
        m_materialSetLayout->GetHandle(),
        geometryConfig);
    // The default config is the forward shape: triangle.frag, one HDR attachment, RGB writes.
    m_forwardPipelines = std::make_unique<VulkanPipelineSet>(
        m_device->GetHandle(),
        m_pipelineCache,
        forwardPass->GetRenderPass(),
        m_frameSetLayout->GetHandle(),
        m_materialSetLayout->GetHandle(),
        MaterialPipelineSetConfig{});

    // A rebuilt exposure pass starts with zeroed histograms, which meter as empty, so auto
    // exposure holds its current EV for the kMaxFramesInFlight frames until real ones arrive.
    auto exposurePass = std::make_unique<VulkanExposureHistogramPass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets);
    m_exposurePass = exposurePass.get();

    // Construction order does not matter: RecordScenePasses follows BuildScenePassOrder.
    m_scenePasses.push_back(std::move(geometryPass));
    m_scenePasses.push_back(std::make_unique<VulkanLightingPass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle(),
        m_gbufferDescriptors->GetEmptySetLayout(),
        m_gbufferDescriptors->GetSetLayout()));
    m_scenePasses.push_back(std::move(forwardPass));
    m_scenePasses.push_back(std::move(exposurePass));
    m_scenePasses.push_back(std::make_unique<VulkanTonemapPass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_gbufferDescriptors->GetSetLayout(),
        m_gbufferDescriptors->GetEmptySetLayout()));
}

IScenePass* VulkanRenderer::FindScenePass(ScenePassId id) const
{
    for (const std::unique_ptr<IScenePass>& pass : m_scenePasses)
    {
        if (pass->Id() == id)
        {
            return pass.get();
        }
    }

    // A pass named by an order the renderer built but never constructed is a programming error,
    // and returning null here would surface as a crash inside recording instead.
    throw std::runtime_error(
        "No scene pass is registered for scene pass id " +
        std::to_string(static_cast<int32_t>(id)));
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
        BuildMaterialTextureBindings(ViewTextures(m_textures), m_materialTextureSlots),
        m_shadowPass->GetSampledBinding(),
        static_cast<uint32_t>(m_renderSubmeshes.size()));
}

void VulkanRenderer::DestroyDescriptorResources()
{
    m_uniformBuffer.reset();
    // m_textureCacheKeys is deliberately not cleared here: it stays index-paired with m_textures,
    // which survives this teardown, and wiping it makes the next UploadSceneResources upload every
    // live texture again instead of reusing it.
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
    m_gbufferDescriptors->OnTargetsRebuilt(*m_sceneTargets);
    for (const std::unique_ptr<IScenePass>& pass : m_scenePasses)
    {
        pass->OnTargetsRebuilt(*m_sceneTargets);
    }
    m_layoutTracker.Reset();
    m_motionHistory.Reset();
    LOG_INFO(
        "Scene render targets resized to {}x{}",
        m_sceneTargets->GetExtent().width,
        m_sceneTargets->GetExtent().height);
}

void VulkanRenderer::UploadSceneResources()
{
    // Live textures are reused by cache key rather than uploaded again, which keeps peak memory at
    // one copy of an unchanged texture set. They are only looked up here: nothing leaves
    // m_textures until ApplyRenderContent commits, so a throw anywhere below (running out of GPU
    // memory, typically) unwinds this upload's own new resources and leaves every live texture
    // exactly where the current descriptor sets expect it.
    std::unordered_map<std::string, size_t> liveTextureByKey;
    for (size_t i = 0; i < m_textures.size() && i < m_textureCacheKeys.size(); ++i)
    {
        if (m_textures[i] && !m_textureCacheKeys[i].empty())
        {
            liveTextureByKey.emplace(m_textureCacheKeys[i], i);
        }
    }

    std::vector<PendingTexture> newTextures;
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

    // Material texture files are uploaded block-compressed whenever the device allows it.
    const bool compressTextures = m_device->SupportsBlockCompression();
    size_t texturesFromCache = 0;
    size_t texturesCompressedNow = 0;
    size_t texturesUncompressed = 0;
    double compressSecondsTotal = 0.0;
    auto uploadPrepared = [&](const PreparedTexture& prepared, TextureUsage usage) -> std::unique_ptr<VulkanTexture>
    {
        if (!prepared.compressed)
        {
            ++texturesUncompressed;
            return std::make_unique<VulkanTexture>(
                m_device->GetPhysicalDevice(), m_device->GetHandle(),
                prepared.rgba, uploadBatch, ToVulkanTextureFormat(usage));
        }
        if (prepared.fromCache)
        {
            ++texturesFromCache;
        }
        else
        {
            ++texturesCompressedNow;
            compressSecondsTotal += prepared.compressSeconds;
        }
        return std::make_unique<VulkanTexture>(
            m_device->GetPhysicalDevice(), m_device->GetHandle(),
            *prepared.compressed, uploadBatch);
    };

    // Appends a reference to the live texture with this key, if there is one, and returns its new
    // index. The live texture stays in m_textures; ApplyRenderContent moves it across on commit.
    auto reuseLiveTexture = [&](const std::string& key) -> std::optional<uint32_t>
    {
        const auto liveIt = liveTextureByKey.find(key);
        if (liveIt == liveTextureByKey.end())
        {
            return std::nullopt;
        }
        const uint32_t idx = static_cast<uint32_t>(newTextures.size());
        newTextures.push_back(PendingTexture{nullptr, liveIt->second});
        newCacheKeys.push_back(key);
        keyToIndex.emplace(key, idx);
        return idx;
    };

    // Acquire a built-in texture by cache key: reuse the live one when there is one, else upload.
    auto acquireDefault = [&](const std::string& id, const TextureData& data, VulkanTextureFormat fmt) -> uint32_t
    {
        const std::string key = id + (fmt == VulkanTextureFormat::SrgbColor ? "|srgb" : "|linear");
        if (auto it = keyToIndex.find(key); it != keyToIndex.end())
            return it->second;
        if (const std::optional<uint32_t> reused = reuseLiveTexture(key))
            return *reused;

        const uint32_t idx = static_cast<uint32_t>(newTextures.size());
        newTextures.push_back(PendingTexture{std::make_unique<VulkanTexture>(
            m_device->GetPhysicalDevice(), m_device->GetHandle(),
            data, uploadBatch, fmt)});
        newCacheKeys.push_back(key);
        keyToIndex.emplace(key, idx);
        flushUploadBatchIfNeeded();
        return idx;
    };

    auto loadTextureIndex = [&](const std::string& texturePath, TextureUsage usage, uint32_t fallbackIndex) -> uint32_t
    {
        if (texturePath.empty())
            return fallbackIndex;

        const std::string key = BuildTextureCacheKey(texturePath, usage);
        if (auto it = keyToIndex.find(key); it != keyToIndex.end())
            return it->second;

        // Live hit: reuse the existing GPU texture, with no disk I/O, no upload and no extra memory.
        if (const std::optional<uint32_t> reused = reuseLiveTexture(key))
            return *reused;

        // Miss: load from disk and upload. A texture that cannot be decoded falls back to the
        // default, but running out of memory fails the whole upload: substituting white for a
        // texture the GPU had no room for would hide the problem and still leave no room for the
        // geometry that follows.
        try
        {
            const uint32_t idx = static_cast<uint32_t>(newTextures.size());
            newTextures.push_back(PendingTexture{uploadPrepared(PrepareTexture(texturePath, usage, compressTextures), usage)});
            newCacheKeys.push_back(key);
            keyToIndex.emplace(key, idx);
            flushUploadBatchIfNeeded();
            return idx;
        }
        catch (const std::exception& error)
        {
            if (IsOutOfMemoryError(error))
            {
                throw;
            }
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
            TextureUsage usage = TextureUsage::Color;
            PreparedTexture prepared;
            bool decodeFailed = false;
            std::string decodeError;
        };

        std::vector<PendingTextureLoad> pendingLoads;
        std::unordered_set<std::string> seenKeys;
        auto considerPath = [&](const std::string& path, TextureUsage usage)
        {
            if (path.empty())
            {
                return;
            }
            const std::string key = BuildTextureCacheKey(path, usage);
            if (liveTextureByKey.count(key) != 0 || !seenKeys.insert(key).second)
            {
                return;
            }
            pendingLoads.push_back(PendingTextureLoad{key, path, usage, {}, false, {}});
        };

        for (const CpuRenderSubmesh& cpuRenderSubmesh : State().rendererWorld.GetRenderSubmeshes())
        {
            if (!cpuRenderSubmesh.hasTexCoords)
            {
                continue;
            }
            const MaterialTexturePaths& textures = cpuRenderSubmesh.textures;
            considerPath(textures.baseColor, TextureUsage::Color);
            considerPath(textures.normal, TextureUsage::Normal);
            considerPath(textures.metallic, TextureUsage::Data);
            considerPath(textures.roughness, TextureUsage::Data);
            considerPath(textures.occlusion, TextureUsage::Data);
            considerPath(textures.emissive, TextureUsage::Color);
            considerPath(textures.secondaryBaseColor, TextureUsage::Color);
            considerPath(textures.secondaryNormal, TextureUsage::Normal);
            considerPath(textures.secondaryMetallic, TextureUsage::Data);
            considerPath(textures.secondaryRoughness, TextureUsage::Data);
            considerPath(textures.secondaryOcclusion, TextureUsage::Data);
            considerPath(textures.secondaryEmissive, TextureUsage::Color);
            considerPath(textures.blendMask, TextureUsage::Data);
        }

        // Prepare in bounded chunks rather than all at once, so we don't hold dozens of huge
        // decoded RGBA8 buffers in host memory simultaneously (a 40 MB source PNG can decode to
        // 100+ MB of raw pixels, and compressing it holds its mip chain as well). One task per
        // hardware thread: compression keeps each of them busy.
        const size_t chunkSize = std::max<size_t>(4, static_cast<size_t>(std::thread::hardware_concurrency()));
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
                decodeTasks.push_back(std::async(std::launch::async, [&pending = pendingLoads[i], compressTextures]()
                                                 {
                                                     try
                                                     {
                                                         pending.prepared = PrepareTexture(pending.path, pending.usage, compressTextures);
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
                    newTextures.push_back(PendingTexture{uploadPrepared(pending.prepared, pending.usage)});
                    newCacheKeys.push_back(pending.cacheKey);
                    keyToIndex.emplace(pending.cacheKey, idx);
                    flushUploadBatchIfNeeded();
                }
                catch (const std::exception& error)
                {
                    // As in loadTextureIndex: a bad texture falls back, running out of memory
                    // fails the upload.
                    if (IsOutOfMemoryError(error))
                    {
                        throw;
                    }
                    LOG_ERROR("Failed to upload prefetched model texture '{}': {}", pending.path, error.what());
                }

                // Release the prepared pixels promptly instead of waiting for pendingLoads itself
                // to go out of scope at the end of the prefetch block.
                pending.prepared = PreparedTexture{};
            }
        }
    }

    for (const CpuRenderSubmesh& cpuRenderSubmesh : State().rendererWorld.GetRenderSubmeshes())
    {
        RenderSubmesh renderSubmesh{};
        renderSubmesh.entity = cpuRenderSubmesh.entity;
        renderSubmesh.buffer = std::make_unique<VulkanBuffer>(
            m_device->GetPhysicalDevice(), m_device->GetHandle(),
            *cpuRenderSubmesh.mesh, uploadBatch);
        flushUploadBatchIfNeeded();
        renderSubmesh.material = cpuRenderSubmesh.material;
        renderSubmesh.doubleSided = cpuRenderSubmesh.doubleSided;
        renderSubmesh.alphaMode = cpuRenderSubmesh.alphaMode;
        renderSubmesh.localBoundsCenter = cpuRenderSubmesh.localBoundsCenter;
        renderSubmesh.localBoundsRadius = cpuRenderSubmesh.localBoundsRadius;
        renderSubmesh.name = cpuRenderSubmesh.name;

        if (!cpuRenderSubmesh.hasTexCoords)
        {
            renderSubmesh.materialBindingIndex = defaultMaterialBindingIndex;
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
            continue;
        }

        MaterialTextureSlots slots = newMaterialTextureSlots[defaultMaterialBindingIndex];
        slots.baseColor = loadTextureIndex(cpuRenderSubmesh.textures.baseColor, TextureUsage::Color, defaultBaseColorIndex);
        slots.normal = loadTextureIndex(cpuRenderSubmesh.textures.normal, TextureUsage::Normal, defaultNormalIndex);
        slots.metallic = loadTextureIndex(cpuRenderSubmesh.textures.metallic, TextureUsage::Data, defaultMetallicIndex);
        slots.roughness = loadTextureIndex(cpuRenderSubmesh.textures.roughness, TextureUsage::Data, defaultRoughnessIndex);
        slots.occlusion = loadTextureIndex(cpuRenderSubmesh.textures.occlusion, TextureUsage::Data, defaultOcclusionIndex);
        slots.emissive = loadTextureIndex(cpuRenderSubmesh.textures.emissive, TextureUsage::Color, defaultEmissiveIndex);
        slots.secondaryBaseColor = loadTextureIndex(cpuRenderSubmesh.textures.secondaryBaseColor, TextureUsage::Color, slots.baseColor);
        slots.secondaryNormal = loadTextureIndex(cpuRenderSubmesh.textures.secondaryNormal, TextureUsage::Normal, slots.normal);
        slots.secondaryMetallic = loadTextureIndex(cpuRenderSubmesh.textures.secondaryMetallic, TextureUsage::Data, slots.metallic);
        slots.secondaryRoughness = loadTextureIndex(cpuRenderSubmesh.textures.secondaryRoughness, TextureUsage::Data, slots.roughness);
        slots.secondaryOcclusion = loadTextureIndex(cpuRenderSubmesh.textures.secondaryOcclusion, TextureUsage::Data, slots.occlusion);
        slots.secondaryEmissive = loadTextureIndex(cpuRenderSubmesh.textures.secondaryEmissive, TextureUsage::Color, slots.emissive);
        slots.blendMask = loadTextureIndex(cpuRenderSubmesh.textures.blendMask, TextureUsage::Data, defaultBlendMaskIndex);

        renderSubmesh.materialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
        newMaterialTextureSlots.push_back(slots);
        newRenderSubmeshes.push_back(std::move(renderSubmesh));
    }
    uploadBatch.Flush();
    LOG_INFO("Uploaded {} submesh buffers and {} textures", newRenderSubmeshes.size(), newTextures.size());
    if (texturesFromCache + texturesCompressedNow + texturesUncompressed > 0)
    {
        LOG_INFO(
            "Texture files: {} block-compressed from the cache, {} compressed now ({:.1f} s of encoding across threads), {} uncompressed",
            texturesFromCache,
            texturesCompressedNow,
            compressSecondsTotal,
            texturesUncompressed);
    }

    ApplyRenderContent(
        std::move(newTextures),
        std::move(newCacheKeys),
        std::move(newMaterialTextureSlots),
        std::move(newRenderSubmeshes));
}

void VulkanRenderer::UploadSceneResourcesOrKeepPrevious()
{
    // What the editor shows while a change is missing from the screen. Kept as one constant so
    // a later successful upload can tell its own report apart from other load errors.
    static constexpr const char* kOutOfMemoryReport =
        "Not enough GPU memory to show the latest scene change. The scene on screen is from before it; "
        "remove models to free memory, and the next change will try again.";

    try
    {
        UploadSceneResources();
    }
    catch (const std::exception& error)
    {
        if (!IsOutOfMemoryError(error))
        {
            throw;
        }
        LOG_ERROR("Keeping the previous scene content, the upload ran out of GPU memory: {}", error.what());
        State().lastModelLoadError = kOutOfMemoryReport;
        DropSubmeshesOfRemovedEntities();
        return;
    }

    // The screen matches the scene again, so a report of it not matching is now stale.
    if (State().lastModelLoadError == kOutOfMemoryReport)
    {
        State().lastModelLoadError.clear();
    }
}

void VulkanRenderer::DropSubmeshesOfRemovedEntities()
{
    const ISceneWorld& sceneWorld = State().rendererWorld.GetSceneWorld();
    const auto isRemoved = [&sceneWorld](const RenderSubmesh& renderSubmesh)
    {
        return !sceneWorld.IsValidEntity(renderSubmesh.entity);
    };
    if (std::none_of(m_renderSubmeshes.begin(), m_renderSubmeshes.end(), isRemoved))
    {
        return;
    }

    // The frames in flight may still draw from the buffers about to be destroyed. What remains is
    // a subset of the list the uniform buffer was sized for, so its motion slots still cover it;
    // material binding indices and motion keys are per submesh and stay valid.
    m_commandContext->WaitForAllFrames();
    std::erase_if(m_renderSubmeshes, isRemoved);
}

void VulkanRenderer::ApplyRenderContent(
    std::vector<PendingTexture> newTextures,
    std::vector<std::string> newTextureCacheKeys,
    std::vector<MaterialTextureSlots> newMaterialTextureSlots,
    std::vector<RenderSubmesh> newRenderSubmeshes)
{
    // Everything before the uniform buffer swap may throw and must leave the renderer untouched;
    // everything after it only moves ownership.
    std::vector<const VulkanTexture*> textureViews;
    textureViews.reserve(newTextures.size());
    for (const PendingTexture& texture : newTextures)
    {
        textureViews.push_back(texture.created ? texture.created.get() : m_textures.at(texture.reusedIndex).get());
    }

    // A submesh's motion key is its entity and its position among that entity's submeshes, so a
    // reload that reorders the list still finds each draw's own history.
    std::unordered_map<uint32_t, uint32_t> nextSubmeshOrdinal;
    for (RenderSubmesh& renderSubmesh : newRenderSubmeshes)
    {
        const uint32_t entity = static_cast<uint32_t>(entt::to_integral(renderSubmesh.entity));
        renderSubmesh.motionKey = MotionKey{entity, nextSubmeshOrdinal[entity]++};
    }

    std::unique_ptr<VulkanUniformBuffer> newUniformBuffer;

    if (m_swapchain && m_renderPass && !m_scenePasses.empty() && !newTextures.empty() && !newMaterialTextureSlots.empty())
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
            BuildMaterialTextureBindings(textureViews, newMaterialTextureSlots),
            m_shadowPass->GetSampledBinding(),
            static_cast<uint32_t>(newRenderSubmeshes.size()));
        // Wait only for our in-flight render frames to finish before destroying old resources.
        // vkWaitForFences is more targeted than vkDeviceWaitIdle: it doesn't stall the
        // present or transfer queues, and the new UBO above is built while the GPU may still
        // be executing the previous frame (overlapping CPU and GPU work).
        m_commandContext->WaitForAllFrames();
        m_uniformBuffer = std::move(newUniformBuffer);
    }

    // Commit. Reused textures move across from the live list; whatever is left behind in it is no
    // longer referenced and is destroyed at the end of this function, after the wait above and
    // after the old descriptor sets went with the old uniform buffer. Destroying it earlier would
    // free image views the GPU may still sample (validation reports it as
    // vkDestroySampler-while-in-use).
    std::vector<std::unique_ptr<VulkanTexture>> textures;
    textures.reserve(newTextures.size());
    for (PendingTexture& texture : newTextures)
    {
        textures.push_back(texture.created ? std::move(texture.created) : std::move(m_textures.at(texture.reusedIndex)));
    }
    std::vector<std::unique_ptr<VulkanTexture>> retiredTextures = std::move(m_textures);

    m_textures = std::move(textures);
    m_textureCacheKeys = std::move(newTextureCacheKeys);
    m_materialTextureSlots = std::move(newMaterialTextureSlots);
    m_renderSubmeshes = std::move(newRenderSubmeshes);
}

std::vector<VulkanDrawItem> VulkanRenderer::BuildDrawItems(uint32_t imageIndex, std::span<const glm::mat4> models) const
{
    std::vector<VulkanDrawItem> unsorted;
    std::vector<MaterialDrawSortKey> sortKeys;
    unsorted.reserve(m_renderSubmeshes.size());
    sortKeys.reserve(m_renderSubmeshes.size());

    for (size_t submeshIndex = 0; submeshIndex < m_renderSubmeshes.size(); ++submeshIndex)
    {
        const RenderSubmesh& renderSubmesh = m_renderSubmeshes[submeshIndex];
        ObjectPushConstants drawConstants{};
        drawConstants.model = models[submeshIndex];
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
            pipelineKey,
            // The slot is the submesh index, which is also where DrawFrame put this submesh's
            // previous model matrix.
            static_cast<uint32_t>(submeshIndex)});
    }

    std::vector<VulkanDrawItem> ordered;
    ordered.reserve(unsorted.size());
    for (size_t index : BuildMaterialDrawOrder(sortKeys))
    {
        ordered.push_back(std::move(unsorted[index]));
    }
    return ordered;
}

std::vector<ShadowDrawItem> VulkanRenderer::BuildShadowDrawItems(uint32_t imageIndex) const
{
    std::vector<ShadowDrawItem> items;
    items.reserve(m_renderSubmeshes.size());
    for (const RenderSubmesh& renderSubmesh : m_renderSubmeshes)
    {
        // Blend materials are glass, foliage cards and the like; a solid shadow from them would be
        // wrong more often than none, so they cast none.
        if (renderSubmesh.alphaMode == MaterialAlphaMode::Blend)
        {
            continue;
        }

        ShadowDrawItem item{};
        item.vertexBuffer = renderSubmesh.buffer->GetVertexHandle();
        item.indexBuffer = renderSubmesh.buffer->GetIndexHandle();
        item.indexCount = renderSubmesh.buffer->GetIndexCount();
        item.model = State().rendererWorld.GetModelMatrix(renderSubmesh.entity);
        item.worldBoundsCenter = glm::vec3(item.model * glm::vec4(renderSubmesh.localBoundsCenter, 1.0f));
        // The largest axis scale keeps the sphere enclosing under non-uniform scale.
        item.worldBoundsRadius =
            renderSubmesh.localBoundsRadius *
            std::max({glm::length(glm::vec3(item.model[0])),
                      glm::length(glm::vec3(item.model[1])),
                      glm::length(glm::vec3(item.model[2]))});
        item.alphaMask = renderSubmesh.alphaMode == MaterialAlphaMode::Mask;
        item.materialDescriptorSet = m_uniformBuffer->GetDescriptorSet(imageIndex, renderSubmesh.materialBindingIndex);
        item.material = renderSubmesh.material;
        items.push_back(item);
    }
    // Opaque first, then mask, so the pass switches pipeline once.
    std::stable_partition(
        items.begin(),
        items.end(),
        [](const ShadowDrawItem& item)
        {
            return !item.alphaMask;
        });
    return items;
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

void VulkanRenderer::RecordScenePasses(
    VkCommandBuffer commandBuffer,
    const ScenePassFrameContext& frame,
    std::span<const ScenePassId> passOrder)
{
    // The layout tracker is reset by the caller, at the head of the command buffer it describes.
    // Only the passes in this frame's order record, but every owned pass follows a target rebuild
    // (see SyncSceneTargets), so flipping the switch never meets a stale framebuffer.
    for (const ScenePassId id : passOrder)
    {
        const IScenePass* pass = FindScenePass(id);
        RecordTransitions(commandBuffer, pass->Io(), frame);
        pass->Record(commandBuffer, *m_sceneTargets, frame);
    }
}

void VulkanRenderer::ReportDroppedLights(uint32_t droppedCount)
{
    // Logged when the count changes rather than every frame, which would bury everything else.
    if (droppedCount == m_droppedLightCount)
    {
        return;
    }
    m_droppedLightCount = droppedCount;
    if (droppedCount > 0)
    {
        LOG_WARN(
            "The scene has {} more non-ambient lights than the {} the renderer evaluates; the dimmest at the camera are left out",
            droppedCount,
            kMaxSceneLights);
    }
    else
    {
        LOG_INFO("Every scene light fits in the renderer's light limit again");
    }
}

void VulkanRenderer::UpdateAutoExposure(uint32_t frameSlot)
{
    Camera& camera = State().camera;
    const AutoExposureSettings& settings = camera.autoExposure;
    if (!settings.enabled || !m_exposurePass)
    {
        // Manual mode: exposureEv100 is the user's. When auto exposure is turned back on it
        // adapts from that value rather than snapping.
        return;
    }

    // The slot's fence has signaled, so its histogram is the one it recorded kMaxFramesInFlight
    // frames ago. Empty means nothing but background was visible; the exposure then holds.
    const std::optional<float> target = MeterTargetEv100(m_exposurePass->GetHistogram(frameSlot), settings);
    if (!target.has_value())
    {
        return;
    }

    camera.exposureEv100 = m_hasMeteredExposure
                               ? AdaptEv100(camera.exposureEv100, *target, State().frameDeltaSeconds, settings)
                               : *target;
    m_hasMeteredExposure = true;
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
