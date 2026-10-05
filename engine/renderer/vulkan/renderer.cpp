#include "renderer.h"

#include "../imgui/imgui_impl_vulkan.h"
#include "viewport_capture.h"

#include <engine/renderer/view_frustum.h>
#include <engine/renderer/environment_brdf.h>
#include <engine/renderer/ltc_table.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/renderer/scene_lighting.h>

#include <engine/logic/editor_world.h>
#include <engine/scene/scene_components.h>
#include <imgui.h>
#include <engine/asset/compressed_texture_cache.h>
#include <engine/asset/texture_preparation.h>
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
#include <cstring>
#include <filesystem>
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

// The scene's light entities, then the lights models carry (KHR_lights_punctual) through their
// entities' transforms.
CollectedSceneLights CollectSceneLights(const IEditorWorld& world, const RendererWorld& rendererWorld)
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
                           // z is the area light's width, or the point or spot light's source radius.
                           gpu.spotAndArea = glm::vec4(
                               innerCos,
                               outerCos,
                               light.type == LightType::Area ? light.areaSize.x * scale.x : std::max(light.sourceRadius, 0.0f),
                               light.areaSize.y * scale.y);

                           SceneLightCandidate candidate{};
                           candidate.type = light.type;
                           candidate.position = transform.translation;
                           candidate.color = light.color;
                           candidate.intensity = light.intensity;
                           candidate.castShadows = light.castShadows;
                           // A hemisphere light's sky is above the axis a directional light would
                           // shine down.
                           candidate.up = -direction;
                           candidate.groundColor = light.groundColor;

                           collected.gpuLights.push_back(gpu);
                           collected.candidates.push_back(candidate);
                       });

    for (const CpuModelLight& modelLight : rendererWorld.GetModelLights())
    {
        // An entity deleted this frame keeps its lights until the renderables refresh.
        if (!world.HasModelComponent(modelLight.entity))
        {
            continue;
        }
        const LightComponent& light = modelLight.light;
        const PlacedModelLight placed =
            PlaceModelLight(rendererWorld.GetModelMatrix(modelLight.entity), modelLight.position, modelLight.direction);

        GpuLightData gpu{};
        gpu.positionAndRange = glm::vec4(placed.position, light.range);
        gpu.colorAndIntensity = glm::vec4(light.color, light.intensity);
        gpu.directionAndType = glm::vec4(placed.direction, static_cast<float>(light.type));
        gpu.areaRightAxis = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        // glTF's punctual lights have no size: z is a zero source radius.
        gpu.spotAndArea = glm::vec4(
            std::cos(glm::radians(light.spotInnerAngleDegrees)),
            std::cos(glm::radians(light.spotOuterAngleDegrees)),
            0.0f,
            0.0f);

        SceneLightCandidate candidate{};
        candidate.type = light.type;
        candidate.position = placed.position;
        candidate.color = light.color;
        candidate.intensity = light.intensity;
        candidate.castShadows = light.castShadows;

        collected.gpuLights.push_back(gpu);
        collected.candidates.push_back(candidate);
    }
    return collected;
}

GpuTextureTransforms BuildGpuTextureTransforms(const MaterialTextureTransforms& transforms)
{
    static_assert(kGpuTextureTransformSlots == kMaterialTextureSlotCount, "one GPU transform per material texture slot");
    GpuTextureTransforms gpu{};
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        ComputeTextureTransformRows(transforms[slot], &gpu.rows[slot * 8], &gpu.rows[slot * 8 + 4]);
    }
    return gpu;
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

// Where compressed material textures are cached between runs.
std::filesystem::path TextureCacheDirectory()
{
    return EnginePaths::CacheRoot() / "textures";
}

// Every material texture file a textured submesh samples, with the usage its slot gives it.
// UploadSceneResources assigns the same slots with the same usages.
template <typename Visit>
void ForEachMaterialTexture(const CpuRenderSubmesh& submesh, Visit&& visit)
{
    const MaterialTexturePaths& textures = submesh.textures;
    visit(textures.baseColor, TextureUsage::Color);
    visit(textures.normal, TextureUsage::Normal);
    visit(textures.metallic, TextureUsage::Data);
    visit(textures.roughness, TextureUsage::Data);
    visit(textures.occlusion, TextureUsage::Data);
    visit(textures.emissive, TextureUsage::Color);
    visit(textures.secondaryBaseColor, TextureUsage::Color);
    visit(textures.secondaryNormal, TextureUsage::Normal);
    visit(textures.secondaryMetallic, TextureUsage::Data);
    visit(textures.secondaryRoughness, TextureUsage::Data);
    visit(textures.secondaryOcclusion, TextureUsage::Data);
    visit(textures.secondaryEmissive, TextureUsage::Color);
    visit(textures.blendMask, TextureUsage::Data);
    visit(textures.clearcoat, TextureUsage::Data);
    visit(textures.clearcoatRoughness, TextureUsage::Data);
    visit(textures.sheenColor, TextureUsage::Color);
    visit(textures.sheenRoughness, TextureUsage::Data);
    visit(textures.anisotropy, TextureUsage::Data);
    visit(textures.specular, TextureUsage::Data);
    visit(textures.specularColor, TextureUsage::Color);
    visit(textures.clearcoatNormal, TextureUsage::Normal);
    visit(textures.iridescence, TextureUsage::Data);
    visit(textures.iridescenceThickness, TextureUsage::Data);
    visit(textures.transmission, TextureUsage::Data);
    visit(textures.thickness, TextureUsage::Data);
    visit(textures.diffuseTransmission, TextureUsage::Data);
    visit(textures.diffuseTransmissionColor, TextureUsage::Color);
    // Combined as sRGB-encoded values (detail_layers.glsl), so read undecoded.
    visit(textures.detailMask, TextureUsage::Data);
    for (const std::string& layer : textures.detailLayers)
    {
        visit(layer, TextureUsage::Data);
    }
}

// What the editor shows while a change is missing from the screen. Kept as one constant so a later
// successful upload can tell its own report apart from other load errors.
constexpr const char* kOutOfMemoryReport =
    "Not enough GPU memory to show the latest scene change. The scene on screen is from before it; "
    "remove models to free memory, and the next change will try again.";

// Logs a frame long enough to have stalled the editor. Texture work belongs on the preparation
// queue; this is where a regression back onto the frame loop shows up.
// A key that changes whenever the shadow casters do: which meshes, where, and how they alpha test.
// FNV-1a over the fields that reach the shadow map. Not the material descriptor set, which is one
// per swapchain image and would change the key every frame.
uint64_t HashShadowCasters(std::span<const ShadowDrawItem> items)
{
    // Eight bytes a step: a byte at a time took milliseconds over a map's tens of thousands of casters.
    uint64_t hash = 14695981039346656037ull;
    const auto mixWord = [&hash](uint64_t word)
    {
        hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash *= 0xff51afd7ed558ccdull;
    };
    const auto mix = [&mixWord](const void* data, size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        size_t index = 0;
        for (; index + sizeof(uint64_t) <= size; index += sizeof(uint64_t))
        {
            uint64_t word = 0;
            std::memcpy(&word, bytes + index, sizeof(word));
            mixWord(word);
        }
        uint64_t tail = 0;
        std::memcpy(&tail, bytes + index, size - index);
        mixWord(tail ^ (static_cast<uint64_t>(size) << 56));
    };
    for (const ShadowDrawItem& item : items)
    {
        mix(&item.positionBuffer, sizeof(item.positionBuffer));
        mix(&item.vertexBuffer, sizeof(item.vertexBuffer));
        mix(&item.indexBuffer, sizeof(item.indexBuffer));
        mix(&item.indexCount, sizeof(item.indexCount));
        mix(&item.model, sizeof(item.model));
        mix(&item.alphaMask, sizeof(item.alphaMask));
        if (item.alphaMask)
        {
            mix(&item.material, sizeof(item.material));
            mix(item.baseColorTransform, sizeof(item.baseColorTransform));
        }
    }
    return hash;
}

class FrameStallReporter
{
  public:
    ~FrameStallReporter()
    {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
        if (seconds > 1.0)
        {
            LOG_WARN("A frame took {:.1f} s", seconds);
        }
    }

  private:
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

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
    const std::vector<MaterialTextureSlots>& materialTextureSlots,
    VulkanSamplerCache& samplerCache)
{
    std::vector<MaterialTextureBinding> bindings;
    bindings.reserve(materialTextureSlots.size());

    for (const MaterialTextureSlots& slots : materialTextureSlots)
    {
        // The texture's view with the sampler its slot asks for; slot is the binding's index.
        const auto bind = [&](uint32_t textureIndex, uint32_t slot)
        {
            return TextureDescriptorBinding{textures[textureIndex]->GetImageView(), samplerCache.Get(slots.samplers[slot])};
        };
        // The detail maps come outside glTF's texture slots: always the default sampler (repeat,
        // linear, mipmapped), which their tiling needs.
        const auto bindDefault = [&](uint32_t textureIndex)
        {
            return TextureDescriptorBinding{textures[textureIndex]->GetImageView(), samplerCache.Get(TextureSampler{})};
        };
        bindings.push_back(MaterialTextureBinding{
            bind(slots.baseColor, 0),
            bind(slots.normal, 1),
            bind(slots.metallic, 2),
            bind(slots.roughness, 3),
            bind(slots.occlusion, 4),
            bind(slots.emissive, 5),
            bind(slots.secondaryBaseColor, 6),
            bind(slots.secondaryNormal, 7),
            bind(slots.secondaryMetallic, 8),
            bind(slots.secondaryRoughness, 9),
            bind(slots.secondaryOcclusion, 10),
            bind(slots.secondaryEmissive, 11),
            bind(slots.blendMask, 12),
            bind(slots.clearcoat, 13),
            bind(slots.clearcoatRoughness, 14),
            bind(slots.sheenColor, 15),
            bind(slots.sheenRoughness, 16),
            bind(slots.anisotropy, 17),
            bind(slots.specular, 18),
            bind(slots.specularColor, 19),
            bind(slots.clearcoatNormal, 20),
            bind(slots.iridescence, 21),
            bind(slots.iridescenceThickness, 22),
            bind(slots.transmission, 23),
            bind(slots.thickness, 24),
            bind(slots.diffuseTransmission, 25),
            bind(slots.diffuseTransmissionColor, 26),
            bindDefault(slots.detailMask),
            {bindDefault(slots.detailLayers[0]), bindDefault(slots.detailLayers[1]), bindDefault(slots.detailLayers[2]),
             bindDefault(slots.detailLayers[3])}});
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
    case VK_IMAGE_LAYOUT_GENERAL:
        // Only the AO storage targets use it, written and read by compute.
        return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
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
    case VK_IMAGE_LAYOUT_GENERAL:
        return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
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
    {
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(m_device->GetPhysicalDevice(), &features);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(m_device->GetPhysicalDevice(), &properties);
        const float maxAnisotropy = features.samplerAnisotropy ? std::min(16.0f, properties.limits.maxSamplerAnisotropy) : 0.0f;
        m_samplerCache = std::make_unique<VulkanSamplerCache>(m_device->GetHandle(), maxAnisotropy);
    }
    CreateDeviceResources();
    // Half the hardware threads: the rest stay free for the frame loop and for the band-parallel
    // encoding inside each texture.
    const bool compressTextures = m_device->SupportsBlockCompression();
    m_texturePreparation = std::make_unique<TexturePreparationQueue>(
        [compressTextures, cacheDirectory = TextureCacheDirectory()](const std::string& path, TextureUsage usage)
        {
            return PrepareTexture(path, usage, compressTextures, cacheDirectory);
        },
        std::max(1u, std::thread::hardware_concurrency() / 2));
    CreateSwapchainResources();
    // The startup scene uploads synchronously: there is nothing on screen to keep responsive yet,
    // and it has no texture files.
    UploadSceneResources();
}

VulkanRenderer::~VulkanRenderer()
{
    // Its last frames are read back from the device about to be torn down.
    StopVideoRecording();
    // Joins the workers before anything they might still be preparing for is torn down.
    m_texturePreparation.reset();

    if (m_device)
    {
        vkDeviceWaitIdle(m_device->GetHandle());
    }
    m_videoReadback.reset();

    DestroyDescriptorResources();
    m_forwardPipelines.reset();
    m_geometryPipelines.reset();
    m_decalPipelines.reset();
    DestroySwapchainResources();
    m_scenePasses.clear();
    m_exposurePass = nullptr;
    m_gbufferDescriptors.reset();
    m_sceneTargets.reset();
    // Its ImGui binding goes before the ImGui backend that made it.
    ReleaseMinimapTexture();
    m_imguiLayer.reset();
    m_textureStore.clear();
    m_stagedTextures.clear();
    m_renderSubmeshes.clear();
    m_liveSubmeshes.clear();
    m_materialSets.reset();
    m_samplerCache.reset();
    DestroyDeviceResources();
    m_device.reset();
    m_instance.reset();
}

void VulkanRenderer::DrawFrame()
{
    const FrameStallReporter stallReporter;
    const auto frameStart = std::chrono::steady_clock::now();
    m_cpuStages.BeginFrame();

    if (!TickSharedFrame())
    {
        return;
    }
    m_cpuStages.Mark("Tick");

    const bool contentChanged = ProcessPendingOperations();
    m_cpuStages.Mark("PendingOperations");
    if (contentChanged)
    {
        RequestSceneUpload();
        m_cpuStages.Mark("RequestUpload");
    }
    // Every frame: stages textures the workers finished, and commits a pending change once its
    // last texture is ready.
    PumpSceneUpload();
    m_cpuStages.Mark("PumpUpload");

    EditorWorld().FlushDirtyTransforms();
    m_cpuStages.Mark("SceneUpdates");

    // A swapchain that no longer matches the window is rebuilt before drawing rather than after a
    // present reports it: drawing into the old size first leaves the newly exposed area unpainted
    // for a frame, which shows on every step of a live resize.
    // Switching HDR output changes the swapchain's format, and with it everything built on it.
    if (SwapchainNeedsResize() || State().renderDebug.hdrOutput != m_swapchainHdrRequested)
    {
        RecreateSwapchain();
    }

    SyncSceneTargets();

    uint32_t imageIndex = 0;
    const auto waitStart = std::chrono::steady_clock::now();
    const VkResult acquireResult = m_commandContext->AcquireNextImage(m_swapchain->GetHandle(), imageIndex);
    const double waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    m_cpuStages.Mark("Acquire");
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        RecreateSwapchain();
        return;
    }

    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        CheckVulkan(acquireResult, "Failed to acquire swapchain image");
    }
    // AcquireNextImage waited on this slot's fence: the video frame its last use copied is ready.
    SubmitVideoFrame(m_commandContext->GetCurrentFrame());

    UpdateAutoExposure(m_commandContext->GetCurrentFrame());
    UpdateViewportMatrices(FromVkExtent(m_sceneTargets->GetExtent()));

    m_imguiLayer->BeginFrame();
    State().editorUi.BeginFrame(GetWindow().GetSDLWindow(), State().engineSettings);
    // ImGui samples the tone mapped image, which is the LDR target and so is indexed by
    // swapchain image: its texture binding is handed out here, before the command buffer that
    // writes it is recorded.
    UpdateMinimapTexture();
    const EditorUiFrameResult uiFrame = DrawEditorUi(
        m_sceneTargets->GetLdrTextureId(imageIndex),
        FromVkExtent(m_sceneTargets->GetExtent()));
    ApplyUiActions(uiFrame);
    EditorWorld().FlushDirtyTransforms();
    if (State().renderablesDirty)
    {
        // A change that needs no new texture file commits right here, in this frame.
        RequestSceneUpload();
        PumpSceneUpload();
        State().renderablesDirty = false;
    }
    ImGui::Render();
    m_cpuStages.Mark("EditorUi");

    const CollectedSceneLights sceneLights =
        State().editorWorld ? CollectSceneLights(*State().editorWorld, State().rendererWorld) : CollectedSceneLights{};
    const SceneLightSelection lightSelection =
        SelectSceneLights(sceneLights.candidates, State().camera.position, kMaxSceneLights);
    ReportDroppedLights(lightSelection.droppedCount);
    std::vector<GpuLightData> selectedLights;
    selectedLights.reserve(lightSelection.selected.size());
    uint32_t directionalLightCount = 0;
    for (uint32_t index : lightSelection.selected)
    {
        selectedLights.push_back(sceneLights.gpuLights[index]);
        if (sceneLights.candidates[index].type == LightType::Directional)
        {
            ++directionalLightCount;
        }
    }

    ShadowUniformData shadowData{};
    std::optional<ShadowCascades> shadowCascades;
    const int32_t shadowLightIndex = SelectShadowCasterLight(sceneLights.candidates, lightSelection);
    // The Khronos reference view draws no shadows, as the Sample Viewer does not; the caster stays
    // the sun for the sky and exposure below.
    if (shadowLightIndex >= 0 && !State().renderDebug.khronosReference)
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
        shadowSettings.maxDistance = std::clamp(State().renderDebug.shadowDistance, 10.0f, 5000.0f);
        shadowCascades = BuildShadowCascades(
            shadowCamera,
            glm::vec3(selectedLights[shadowLightIndex].directionAndType),
            shadowSettings);

        shadowData.params = glm::vec4(
            static_cast<float>(shadowLightIndex),
            1.0f / static_cast<float>(m_shadowPass->GetResolution()),
            0.0f,
            0.0f);
    }
    // The casters, built here because which cascades the map keeps depends on them. The shader
    // samples the cascades as the map holds them, which for one Plan left waiting is its previous
    // matrix.
    m_cpuStages.Mark("Lights");
    // Every draw's model matrix, once, for the shadow casters, the motion vectors, the ray scene and
    // the draw items.
    std::vector<glm::mat4> models;
    std::vector<MotionKey> motionKeys;
    std::vector<uint32_t> drawSlots;
    models.reserve(m_renderSubmeshes.size());
    motionKeys.reserve(m_renderSubmeshes.size());
    drawSlots.reserve(m_renderSubmeshes.size());
    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
    {
        models.push_back(State().rendererWorld.GetSubmeshModelMatrix(renderSubmesh->entity, renderSubmesh->motionKey.submeshOrdinal));
        motionKeys.push_back(renderSubmesh->motionKey);
        drawSlots.push_back(renderSubmesh->drawSlot);
    }
    m_cpuStages.Mark("Models");
    const std::vector<ShadowDrawItem> shadowDrawItems = BuildShadowDrawItems(imageIndex, models);
    m_cpuStages.Mark("ShadowDrawItems");
    const std::optional<ShadowCascadePlan> shadowPlan =
        m_shadowPass->Plan(shadowCascades.has_value() ? &*shadowCascades : nullptr, HashShadowCasters(shadowDrawItems));
    m_cpuStages.Mark("ShadowPlan");
    if (shadowPlan.has_value())
    {
        for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
        {
            shadowData.cascadeViewProjection[cascade] = shadowPlan->held[cascade].viewProjection;
            shadowData.cascadeSplits[cascade] = shadowPlan->held[cascade].splitFar;
            shadowData.cascadeTexelSizes[cascade] = shadowPlan->held[cascade].texelWorldSize;
        }
    }

    const SceneEnvironment environment = State().editorWorld ? EditorWorld().GetEnvironment() : SceneEnvironment{};
    UpdateEnvironmentMap(environment);
    const EnvironmentMode environmentMode = EffectiveEnvironmentMode(environment);
    const AtmosphereParameters atmosphereParameters = BuildAtmosphereParameters(environment.atmosphere);
    std::optional<AtmosphereSun> sun;
    if (shadowLightIndex >= 0)
    {
        GpuLightData& sunLight = selectedLights[static_cast<size_t>(shadowLightIndex)];
        sun = AtmosphereSun{
            -glm::normalize(glm::vec3(sunLight.directionAndType)),
            glm::vec3(sunLight.colorAndIntensity) * sunLight.colorAndIntensity.w};
        if (environmentMode == EnvironmentMode::Atmosphere)
        {
            // The light's intensity is the illuminance above the atmosphere; the scene receives
            // what gets through to the camera's altitude.
            const glm::vec3 camera = ToAtmosphereCameraPositionKm(atmosphereParameters, State().camera.position);
            const float cosZenith = glm::dot(sun->directionToSun, glm::normalize(camera));
            const glm::vec3 transmittance = ComputeTransmittanceToSpace(
                atmosphereParameters,
                glm::length(camera) - atmosphereParameters.bottomRadiusKm,
                cosZenith);
            sunLight.colorAndIntensity = glm::vec4(glm::vec3(sunLight.colorAndIntensity) * transmittance, sunLight.colorAndIntensity.w);
        }
    }
    // The long-term auto exposure references for the next frame: the sun as it reaches the ground,
    // and the sky's average luminance where the CPU knows it.
    {
        constexpr glm::vec3 kLuma(0.2126f, 0.7152f, 0.0722f);
        m_exposureReferences.sunIlluminanceLux.reset();
        m_whiteBalanceReferences.sunIlluminanceRgb.reset();
        if (shadowLightIndex >= 0)
        {
            const glm::vec4& sunColor = selectedLights[static_cast<size_t>(shadowLightIndex)].colorAndIntensity;
            m_exposureReferences.sunIlluminanceLux = glm::dot(glm::vec3(sunColor) * sunColor.w, kLuma);
            m_whiteBalanceReferences.sunIlluminanceRgb = glm::vec3(sunColor) * sunColor.w;
        }
        m_exposureReferences.skyLuminance.reset();
        m_whiteBalanceReferences.skyIlluminanceRgb.reset();
        if (environmentMode == EnvironmentMode::Atmosphere)
        {
            // The sky SH the atmosphere left in this slot kMaxFramesInFlight frames ago; its fence
            // has signaled.
            if (const std::optional<glm::vec3> sky = m_atmosphere->GetSkyAverageRadiance(m_commandContext->GetCurrentFrame()))
            {
                m_exposureReferences.skyLuminance = glm::dot(*sky, kLuma);
                m_whiteBalanceReferences.skyIlluminanceRgb = *sky * glm::pi<float>();
            }
        }
        else if (environmentMode == EnvironmentMode::None)
        {
            m_exposureReferences.skyLuminance = glm::dot(lightSelection.ambientLuminance, kLuma);
            // A uniform sky of luminance L puts pi L on a horizontal surface.
            m_whiteBalanceReferences.skyIlluminanceRgb = lightSelection.ambientLuminance * glm::pi<float>();
        }
    }
    EnvironmentUniformData environmentData = BuildEnvironmentUniformData(
        environmentMode,
        environment,
        atmosphereParameters,
        sun,
        State().camera.position,
        environmentMode == EnvironmentMode::Hdri ? &m_environmentMapSh : nullptr);
    // The clouds' march jitter steps with the TAA sequence, which averages it; without TAA the
    // index stands still and so does the noise.
    environmentData.cloudParams.w = static_cast<float>(m_taaFrameIndex % 64u);

    if (environmentMode == EnvironmentMode::Hdri)
    {
        // The L0 band of the radiance SH is its average over the sphere times Y00 = 0.282095.
        constexpr glm::vec3 kLuma(0.2126f, 0.7152f, 0.0722f);
        const glm::vec3 averageRadiance = glm::vec3(environmentData.hdriIrradianceSh[0]) * 0.282095f;
        m_exposureReferences.skyLuminance = glm::dot(averageRadiance, kLuma);
        m_whiteBalanceReferences.skyIlluminanceRgb = averageRadiance * glm::pi<float>();
    }

    m_cpuStages.Mark("Environment");
    const glm::mat4 viewProjection = State().viewportMatrices.renderProjection * State().viewportMatrices.view;
    const MotionFrame motion = m_motionHistory.Advance(viewProjection, motionKeys, models);
    m_cpuStages.Mark("Motion");

    // The ray scene: a finished hierarchy build replaces the buffers every frame slot's set names,
    // then this frame's instances go into this slot's.
    if (m_rayScene->HasFinishedBuild())
    {
        m_commandContext->WaitForAllFrames();
        m_rayScene->InstallBuild();
        // The probes hold the old content's light.
        m_ddgi->Invalidate();
        m_ddgiScheduler.Reset();
    }
    const std::span<const uint8_t> ddgiMoving = m_ddgiMovingInstances.Update(models);
    m_rayScene->UpdateInstances(m_commandContext->GetCurrentFrame(), models, ddgiMoving);
    State().rayScenePending = m_rayScene->IsBuilding() || !m_rayScene->IsReady();
    m_cpuStages.Mark("RayInstances");

    // The lighting the probes hold, so they blend faster for a second after it changes: every
    // directional light as it reaches the scene, and the sky's mode, ambient and HDRI.
    std::vector<glm::vec4> ddgiLighting;
    for (size_t index = 0; index < selectedLights.size(); ++index)
    {
        if (sceneLights.candidates[lightSelection.selected[index]].type == LightType::Directional)
        {
            ddgiLighting.push_back(selectedLights[index].directionAndType);
            ddgiLighting.push_back(selectedLights[index].colorAndIntensity);
        }
    }
    ddgiLighting.push_back(glm::vec4(lightSelection.ambientLuminance, static_cast<float>(environmentMode)));
    ddgiLighting.push_back(glm::vec4(environmentMode == EnvironmentMode::Hdri ? m_environmentMapSh[0] : glm::vec3(0.0f), 0.0f));
    const auto now = std::chrono::steady_clock::now();
    const float ddgiSeconds = m_ddgiLastFrameTime ? std::chrono::duration<float>(now - *m_ddgiLastFrameTime).count() : 0.0f;
    m_ddgiLastFrameTime = now;
    const float ddgiHysteresis = m_ddgiHysteresis.Update(ddgiLighting, ddgiSeconds, std::clamp(State().renderDebug.ddgi.hysteresis, 0.0f, 0.999f));

    // DDGI: this frame's levels around the camera and the probes that update. Off in the Khronos
    // reference view, as the Sample Viewer has no GI, and until the ray scene can be traced.
    DdgiUniformData ddgiData{};
    std::vector<uint32_t> ddgiSchedule;
    const DdgiSettings ddgiSettings = State().renderDebug.ddgi;
    if (ddgiSettings.enabled && !State().renderDebug.khronosReference && m_rayScene->IsReady())
    {
        const uint32_t levelCount = static_cast<uint32_t>(std::clamp(ddgiSettings.levels, 1, static_cast<int>(kDdgiMaxLevels)));
        const float baseSpacing = std::clamp(ddgiSettings.baseSpacing, 0.25f, 8.0f);
        const glm::vec2 layout(static_cast<float>(levelCount), baseSpacing);
        if (layout != m_ddgiLayout)
        {
            m_ddgi->Invalidate();
            m_ddgiScheduler.Reset();
            m_ddgiLayout = layout;
        }
        std::array<DdgiLevel, kDdgiMaxLevels> levels{};
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            levels[level] = ComputeDdgiLevel(State().camera.position, std::ldexp(baseSpacing, static_cast<int>(level)));
            ddgiData.spacing[level] = levels[level].spacing;
            ddgiData.origins[level] = glm::vec4(glm::vec3(levels[level].origin), 0.0f);
        }
        const uint32_t budget = static_cast<uint32_t>(std::clamp(ddgiSettings.probesPerFrame, 64, static_cast<int>(VulkanDdgi::kMaxProbesPerFrame)));
        // Levels whose probes have converged refresh less (DdgiProbeScheduler); lighting that
        // changed, or instances that move, start them converging over.
        if (m_ddgiHysteresis.Changed() ||
            std::any_of(ddgiMoving.begin(), ddgiMoving.end(), [](uint8_t moving)
                        {
                            return moving != 0u;
                        }))
        {
            m_ddgiScheduler.Unsettle();
        }
        ddgiSchedule = m_ddgiScheduler.Schedule(std::span<const DdgiLevel>(levels.data(), levelCount), budget, ddgiHysteresis);
        ddgiData.params = glm::vec4(
            static_cast<float>(levelCount),
            static_cast<float>(std::clamp(ddgiSettings.probeViewLevel, 0, static_cast<int>(levelCount) - 1)),
            std::clamp(ddgiSettings.normalBias, 0.0f, 1.0f),
            std::clamp(ddgiSettings.viewBias, 0.0f, 1.0f));
    }
    m_ddgi->SetSchedule(m_commandContext->GetCurrentFrame(), ddgiSchedule);
    m_cpuStages.Mark("DdgiSchedule");

    // TAA jitters what the GPU rasterises, and only that: the editor's matrices and the motion
    // history keep the plain projection, and the camera block carries the plain view-projection for
    // the motion vectors. The forward-only order has no motion vectors, so it never jitters.
    const bool taaEnabled = State().renderDebug.taa && !State().renderDebug.forwardOnly;
    ViewportMatrices renderMatrices = State().viewportMatrices;
    if (taaEnabled)
    {
        const VkExtent2D extent = m_sceneTargets->GetExtent();
        renderMatrices.renderProjection = JitterProjection(
            renderMatrices.renderProjection,
            TaaJitterPixels(m_taaFrameIndex++),
            glm::uvec2(extent.width, extent.height));
    }

    // Local light shadows: the atlas tiles go to the selected local lights in the selection's order,
    // and each light learns its first tile through areaRightAxis.w (1 + tile, 0 for none).
    std::vector<LocalShadowTile> localShadowTiles;
    std::vector<GpuLocalShadowTile> gpuShadowTiles;
    if (State().renderDebug.localLightShadows && !State().renderDebug.khronosReference)
    {
        std::vector<LocalShadowLight> shadowLights;
        shadowLights.reserve(selectedLights.size());
        for (size_t index = 0; index < selectedLights.size(); ++index)
        {
            const GpuLightData& gpu = selectedLights[index];
            const SceneLightCandidate& candidate = sceneLights.candidates[lightSelection.selected[index]];
            LocalShadowLight shadowLight{};
            shadowLight.type = candidate.type;
            shadowLight.position = glm::vec3(gpu.positionAndRange);
            shadowLight.direction = glm::vec3(gpu.directionAndType);
            shadowLight.range = gpu.positionAndRange.w;
            shadowLight.outerAngleRadians = std::acos(std::clamp(gpu.spotAndArea.y, -1.0f, 1.0f));
            shadowLight.castShadows = candidate.castShadows;
            shadowLights.push_back(shadowLight);
        }
        LocalShadowPlan plan = PlanLocalShadows(shadowLights, viewProjection);
        for (size_t index = 0; index < selectedLights.size(); ++index)
        {
            selectedLights[index].areaRightAxis.w = static_cast<float>(plan.firstTile[index] + 1);
        }
        gpuShadowTiles.reserve(plan.tiles.size());
        for (const LocalShadowTile& tile : plan.tiles)
        {
            gpuShadowTiles.push_back(GpuLocalShadowTile{tile.viewProjection, tile.atlasRect, glm::vec4(tile.texelScale, tile.cubeFace ? 1.0f : 0.0f, 0.0f, 0.0f)});
        }
        localShadowTiles = std::move(plan.tiles);
        ReportDroppedLocalShadows(plan.droppedCount);
    }
    else
    {
        ReportDroppedLocalShadows(0);
    }

    // The selection puts every directional light first, so the local lights the grid bins are the
    // tail of selectedLights, and the grid's indices point into the same array the shader reads.
    const bool clusteredLighting = State().renderDebug.clusteredLighting;
    LightClusterGrid lightClusters;
    if (clusteredLighting)
    {
        std::vector<LightClusterSphere> lightSpheres;
        lightSpheres.reserve(selectedLights.size() - directionalLightCount);
        for (uint32_t index = directionalLightCount; index < static_cast<uint32_t>(selectedLights.size()); ++index)
        {
            const glm::vec4& positionAndRange = selectedLights[index].positionAndRange;
            lightSpheres.push_back(LightClusterSphere{glm::vec3(positionAndRange), positionAndRange.w, index});
        }
        LightClusterCamera clusterCamera{};
        clusterCamera.view = State().viewportMatrices.view;
        // The jittered projection: the shader finds a pixel's cluster through the one it rasterised
        // with, and the binning must agree with it.
        clusterCamera.projection = renderMatrices.renderProjection;
        clusterCamera.nearPlane = State().camera.nearPlane;
        clusterCamera.farPlane = State().camera.farPlane;
        lightClusters = BuildLightClusters(clusterCamera, lightSpheres, kLightClusterIndexCapacity);
    }
    ReportDroppedClusterLights(lightClusters.droppedCount);
    LightUpload lightUpload{};
    lightUpload.lights = selectedLights;
    lightUpload.directionalCount = directionalLightCount;
    lightUpload.clusters = clusteredLighting ? &lightClusters : nullptr;
    lightUpload.clustered = clusteredLighting;
    lightUpload.shadowTiles = gpuShadowTiles;

    // This frame's EV, already adapted by UpdateAutoExposure, so every writer and reader of the
    // HDR target agrees on one pre-exposure.
    const float preExposure = PreExposureFromEv100(State().camera.exposureEv100);
    m_uniformBuffer->Update(
        imageIndex,
        renderMatrices,
        State().camera.position,
        lightSelection.ambientLuminance,
        lightSelection.usesFallbackAmbient,
        lightSelection.ambientGradient,
        lightUpload,
        shadowData,
        motion.previousViewProjection,
        motion.previousModels,
        drawSlots,
        environmentData,
        viewProjection,
        // The Sample Viewer does not filter roughness, so the Khronos reference view does not either.
        State().renderDebug.specularAntiAliasing && !State().renderDebug.khronosReference,
        preExposure,
        ddgiData);
    m_cpuStages.Mark("Uniforms");
    // Culled against the jittered projection, the one the GPU rasterises with.
    std::vector<VulkanDrawItem> drawItems =
        BuildDrawItems(imageIndex, models, renderMatrices.renderProjection * renderMatrices.view);
    // The deferred decals leave the Blend tail for the geometry pass, in the same back to front
    // order. The forward-only order has no G-buffer, and a device without independent blending no
    // decal pipelines, so there they stay Blend items.
    std::vector<VulkanDrawItem> decalDrawItems;
    if (!State().renderDebug.forwardOnly && m_decalPipelines)
    {
        const auto decals = std::stable_partition(
            drawItems.begin(),
            drawItems.end(),
            [](const VulkanDrawItem& item)
            {
                return !item.decal;
            });
        decalDrawItems.assign(decals, drawItems.end());
        drawItems.erase(decals, drawItems.end());
    }

    m_cpuStages.Mark("DrawItems");
    ScenePassFrameContext frame{};
    frame.imageIndex = imageIndex;
    frame.frameSlot = m_commandContext->GetCurrentFrame();

    m_referenceFrame.viewProjection = viewProjection;
    m_referenceFrame.cameraPosition = State().camera.position;
    m_referenceFrame.preExposure = preExposure;
    m_referenceFrame.frameSlot = frame.frameSlot;
    m_referenceFrame.view = State().renderDebug.forwardOnly ? GBufferDebugView::Off : State().renderDebug.gbufferView;
    m_referenceFrame.lights.clear();
    for (size_t index = 0; index < selectedLights.size(); ++index)
    {
        if (sceneLights.candidates[lightSelection.selected[index]].type == LightType::Directional)
        {
            const GpuLightData& light = selectedLights[index];
            m_referenceFrame.lights.push_back(ReferenceLight{
                -glm::normalize(glm::vec3(light.directionAndType)),
                glm::vec3(light.colorAndIntensity) * light.colorAndIntensity.w});
        }
    }
    m_referenceFrame.skyRadiance = lightSelection.ambientLuminance;
    m_referenceFrame.uniformSky = environmentMode == EnvironmentMode::None &&
                                  lightSelection.ambientGradient == std::array<glm::vec3, 3>{};
    m_referenceFrame.ddgiEnabled = ddgiData.params.x > 0.0f;
    m_referenceFrame.ddgiLevels = static_cast<uint32_t>(ddgiData.params.x);
    m_referenceFrame.ddgiBias = glm::vec2(ddgiData.params.z, ddgiData.params.w);
    for (uint32_t level = 0; level < kDdgiMaxLevels; ++level)
    {
        m_referenceFrame.ddgiOrigins[level] = glm::ivec3(ddgiData.origins[level]);
        m_referenceFrame.ddgiSpacings[level] = ddgiData.spacing[level];
    }
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
    // The forward-shaded Opaque and Mask draws are the tail of the non-Blend run.
    frame.forwardShadedDrawItemBegin = static_cast<size_t>(
        std::partition_point(
            drawItems.begin(),
            drawItems.begin() + static_cast<std::ptrdiff_t>(frame.blendDrawItemBegin),
            [](const VulkanDrawItem& item)
            {
                return !item.forwardShaded;
            }) -
        drawItems.begin());
    // The transmissive ones are the tail of those, drawn after the transmission copy.
    frame.transmissiveDrawItemBegin = static_cast<size_t>(
        std::partition_point(
            drawItems.begin() + static_cast<std::ptrdiff_t>(frame.forwardShadedDrawItemBegin),
            drawItems.begin() + static_cast<std::ptrdiff_t>(frame.blendDrawItemBegin),
            [](const VulkanDrawItem& item)
            {
                return !item.transmissive;
            }) -
        drawItems.begin());
    frame.forwardPipelines = m_forwardPipelines.get();
    frame.geometryPipelines = m_geometryPipelines.get();
    frame.decalDrawItems = decalDrawItems;
    frame.decalPipelines = m_decalPipelines.get();
    std::vector<VulkanDrawItem> scatterDrawItems;
    for (const VulkanDrawItem& item : drawItems)
    {
        if (item.scatters)
        {
            scatterDrawItems.push_back(item);
        }
    }
    frame.scatterDrawItems = scatterDrawItems;
    frame.scatterPipelines = m_scatterPipelines.get();
    frame.frameDescriptorSet = m_uniformBuffer->GetFrameDescriptorSet(imageIndex);
    frame.gbufferDescriptorSet = m_gbufferDescriptors->GetSet(*m_sceneTargets, imageIndex, frame.frameSlot);
    // The order and the forward filter both derive from this one switch, here, so they cannot
    // disagree. The forward-only order never runs the geometry pass and leaves the G-buffer
    // undefined, so its debug views are forced off rather than trusted to the UI's disabled state.
    const RenderDebugSettings renderDebug = State().renderDebug;
    const std::span<const ScenePassId> passOrder = BuildScenePassOrder(renderDebug.forwardOnly);
    frame.forwardFilter = renderDebug.forwardOnly ? ForwardDrawFilter::All : ForwardDrawFilter::BlendOnly;
    frame.gbufferView = renderDebug.forwardOnly ? GBufferDebugView::Off : renderDebug.gbufferView;
    frame.preExposure = preExposure;
    // The forward-only order runs neither AO pass, so AO is off there by construction. History
    // advances once per recorded frame; a frame that does not accumulate invalidates the next.
    frame.ao = renderDebug.ao;
    frame.ao.enabled = renderDebug.ao.enabled && !renderDebug.forwardOnly && !renderDebug.khronosReference;
    frame.aoHistory = m_aoHistory.Advance(frame.ao.enabled && frame.ao.temporalFilter);
    // The one-bounce indirect diffuse, likewise only in the deferred order; the Khronos reference
    // view has none, as the Sample Viewer.
    frame.gi = renderDebug.gi;
    frame.gi.enabled = renderDebug.gi.enabled && !renderDebug.forwardOnly && !renderDebug.khronosReference;
    frame.giHistory = m_giHistory.Advance(frame.gi.enabled && frame.gi.temporalFilter);
    frame.frameIndex = m_aoFrameIndex++;
    frame.taaEnabled = taaEnabled;
    frame.bloom = renderDebug.bloom;
    // The Khronos reference view renders what the Sample Viewer does: no glare or bloom (nor AO or
    // SSR, above and below).
    frame.khronosReference = renderDebug.khronosReference;
    frame.toneMapper = renderDebug.toneMapper;
    frame.bloom.enabled = renderDebug.bloom.enabled && !renderDebug.khronosReference;

    frame.whiteBalance = UpdateWhiteBalance();
    frame.hdrOutput = m_swapchain->IsHdr();
    frame.hdrPeakNits = std::clamp(renderDebug.hdrPeakNits, 250.0f, 10000.0f);
    // HDR output shows more of the highlight's brightness directly, so it needs less glare.
    frame.glareFNumber = GlareFNumberFromEv100(
        State().camera.exposureEv100,
        frame.hdrOutput ? frame.hdrPeakNits : kGlareSdrPeakNits);
    frame.taaHistory = m_taaHistory.Advance(taaEnabled);
    frame.taaHistoryScale = TaaHistoryScale(frame.taaHistory.valid, preExposure, m_taaHistoryPreExposure);
    // Reflections take their colour from TAA's history, so they trace only where it is valid; the
    // forward-only order has no G-buffer to trace from.
    frame.ssr = renderDebug.ssr;
    frame.ssr.enabled = renderDebug.ssr.enabled && !renderDebug.forwardOnly && !renderDebug.khronosReference;
    frame.ssrHistory = m_ssrHistory.Advance(SsrTraces(frame));
    // The history this frame writes carries this frame's pre-exposure.
    m_taaHistoryPreExposure = preExposure;
    frame.physicalSky = environmentMode != EnvironmentMode::None;
    frame.groundPlane = environmentMode == EnvironmentMode::Atmosphere && environment.atmosphere.groundPlane;

    // A recording takes the tone mapped image the viewport shows, without the editor's overlays,
    // when its video wants a frame for this moment. Frames of another size than the recording's
    // (the targets not yet resized to the size it fixed) are left out.
    const double videoFrameTime = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    VideoRecorder* const videoRecorder = ActiveVideoRecorder();
    const VkExtent2D videoExtent = m_sceneTargets->GetExtent();
    const bool recordVideoFrame =
        videoRecorder != nullptr &&
        videoExtent.width == videoRecorder->GetSettings().width &&
        videoExtent.height == videoRecorder->GetSettings().height &&
        VulkanVideoReadback::SupportsFormat(m_sceneTargets->GetFormat(RenderTargetId::SceneLdr)) &&
        videoRecorder->ClaimFrameAt(videoFrameTime);
    if (recordVideoFrame && !m_videoReadback)
    {
        m_videoReadback = std::make_unique<VulkanVideoReadback>(
            m_device->GetPhysicalDevice(),
            m_device->GetHandle(),
            static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    }

    m_cpuStages.Mark("FrameSetup");
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
                                              m_gpuTimer->BeginFrame(commandBuffer, frame.frameSlot);

                                              // Ahead of the scene passes, whose material pass samples it. It
                                              // orders itself through its render pass dependencies and never
                                              // goes through the layout tracker (see VulkanShadowPass).
                                              m_shadowPass->Record(
                                                  commandBuffer,
                                                  shadowDrawItems,
                                                  shadowPlan.has_value() ? &*shadowPlan : nullptr,
                                                  m_gpuTimer.get());
                                              m_cpuStages.Mark("RecordShadows");
                                              // The same, for the local lights' atlas.
                                              m_localShadowPass->Record(commandBuffer, shadowDrawItems, localShadowTiles);
                                              m_gpuTimer->Mark(commandBuffer, "LocalShadows");
                                              m_cpuStages.Mark("RecordLocalShadows");

                                              // Ahead of the scene passes, whose fragment shaders sample the
                                              // LUTs; it orders itself with its own barriers (see
                                              // VulkanAtmosphere).
                                              m_atmosphere->Record(
                                                  commandBuffer,
                                                  frame.frameDescriptorSet,
                                                  environmentMode == EnvironmentMode::Atmosphere ? &atmosphereParameters : nullptr,
                                                  frame.frameSlot);
                                              m_gpuTimer->Mark(commandBuffer, "Atmosphere");
                                              // After the atmosphere, whose sky-view LUT the capture samples.
                                              m_environmentProbe->Record(
                                                  commandBuffer,
                                                  frame.frameDescriptorSet,
                                                  environmentMode != EnvironmentMode::None,
                                                  environmentData);
                                              m_gpuTimer->Mark(commandBuffer, "EnvironmentProbe");
                                              // The ray materials, when content changed, and the barrier that
                                              // makes the ray scene visible to every trace after it.
                                              m_rayScene->Record(commandBuffer);
                                              m_gpuTimer->Mark(commandBuffer, "RayScene");
                                              // The probes trace the ray scene and must be current before
                                              // any surface samples them.
                                              m_ddgi->Record(
                                                  commandBuffer,
                                                  frame.frameDescriptorSet,
                                                  m_rayScene->GetSet(frame.frameSlot),
                                                  frame.frameSlot,
                                                  m_ddgiFrameIndex++,
                                                  ddgiHysteresis);
                                              m_gpuTimer->Mark(commandBuffer, "Ddgi");

                                              m_cpuStages.Mark("RecordPrePasses");
                                              RecordScenePasses(commandBuffer, frame, passOrder);
                                              m_cpuStages.Mark("RecordScenePasses");

                                              // ImGui samples the tone mapped image in the editor pass, which is
                                              // not an IScenePass because it writes the swapchain rather than a
                                              // scene target.
                                              static constexpr std::array<RenderTargetId, 1> kImGuiReads = {RenderTargetId::SceneLdr};
                                              RenderPassIo imguiIo{};
                                              imguiIo.reads = kImGuiReads;
                                              RecordTransitions(commandBuffer, imguiIo, frame);

                                              RecordEditorLayer(commandBuffer, imageIndex);
                                              m_gpuTimer->Mark(commandBuffer, "ImGui");

                                              if (recordVideoFrame)
                                              {
                                                  // SceneLdr is indexed by swapchain image; the ImGui pass left it
                                                  // shader-read, as CaptureViewport expects to find it.
                                                  const uint32_t ldrIndex = m_sceneTargets->ResolveIndex(RenderTargetId::SceneLdr, imageIndex, 0);
                                                  m_videoReadback->RecordCopy(
                                                      commandBuffer,
                                                      frame.frameSlot,
                                                      m_sceneTargets->GetImage(RenderTargetId::SceneLdr, ldrIndex),
                                                      m_sceneTargets->GetFormat(RenderTargetId::SceneLdr),
                                                      videoExtent,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                      videoFrameTime);
                                              }
                                          });
    m_cpuStages.Mark("RecordRest");
    m_commandContext->Submit(m_device->GetGraphicsQueue(), imageIndex);
    m_cpuStages.Mark("Submit");
    m_lastRecordedImageIndex = imageIndex;
    {
        const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
        const auto push = [this](std::vector<double>& samples, double value)
        {
            if (samples.size() < VulkanGpuTimer::kAverageFrames)
            {
                samples.push_back(value);
            }
            else
            {
                samples[m_cpuFrameCursor % VulkanGpuTimer::kAverageFrames] = value;
            }
        };
        push(m_cpuFrameMs, frameMs - waitMs);
        push(m_cpuWaitMs, waitMs);
        ++m_cpuFrameCursor;
        // A frame the CPU held up for two at 60 Hz says where the time went.
        constexpr double kSlowFrameMs = 33.0;
        if (frameMs - waitMs > kSlowFrameMs)
        {
            std::string stages;
            for (const CpuStageTimer::Stage& stage : m_cpuStages.GetCurrentFrame())
            {
                if (stage.averageMs < 1.0)
                {
                    break;
                }
                stages += (stages.empty() ? "" : ", ") + stage.name + " " + std::to_string(static_cast<int>(stage.averageMs + 0.5)) + " ms";
            }
            LOG_WARN("Slow frame: {:.0f} ms of CPU ({})", frameMs - waitMs, stages);
        }
    }

    const VkResult presentResult = m_commandContext->Present(m_device->GetPresentQueue(), m_swapchain->GetHandle(), imageIndex);
    m_cpuStages.Mark("Present");
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
        supportDetails,
        State().renderDebug.hdrOutput);
    m_swapchainHdrRequested = State().renderDebug.hdrOutput;
    m_renderPass = std::make_unique<VulkanRenderPass>(
        m_device->GetHandle(),
        m_swapchain->GetImageFormat(),
        m_swapchain->GetExtent(),
        m_swapchain->GetImageViews());
    m_commandContext = std::make_unique<VulkanCommandContext>(
        m_device->GetHandle(),
        m_device->GetQueueFamilies(),
        m_renderPass->GetFramebuffers().size());
    m_imguiLayer->CreateOrUpdateVulkanResources(
        m_renderPass->GetHandle(),
        static_cast<uint32_t>(m_swapchain->GetImageViews().size()),
        m_swapchain->IsHdr());
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
    // With HDR output the LDR target holds display-linear values above UI white, which only a float
    // format keeps; ImGui's HDR shader encodes them for the swapchain.
    const VkFormat ldrFormat = m_swapchain->IsHdr() ? VK_FORMAT_R16G16B16A16_SFLOAT : m_swapchain->GetImageFormat();
    const bool ldrFormatMatchesSwapchain =
        m_sceneTargets != nullptr && m_sceneTargets->GetFormat(RenderTargetId::SceneLdr) == ldrFormat;
    if (ldrFormatMatchesSwapchain)
    {
        m_sceneTargets->Rebuild(viewportExtent, swapchainImageCount);
    }
    else
    {
        m_sceneTargets = std::make_unique<SceneRenderTargets>(
            m_device->GetPhysicalDevice(),
            m_device->GetHandle(),
            ldrFormat,
            viewportExtent,
            swapchainImageCount);
    }

    m_layoutTracker.Reset();
    m_motionHistory.Reset();
    m_aoHistory.Reset();
    m_giHistory.Reset();
    m_ssrHistory.Reset();
    m_taaHistory.Reset();
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
    m_scatterPass = nullptr;
    m_forwardPipelines.reset();
    m_scatterPipelines.reset();
    m_geometryPipelines.reset();
    m_decalPipelines.reset();
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
    m_materialSets = std::make_unique<VulkanMaterialSetCache>(m_device->GetHandle(), m_materialSetLayout->GetHandle());

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
    m_localShadowPass = std::make_unique<VulkanLocalShadowPass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        m_materialSetLayout->GetHandle());
    m_transmissionImage = std::make_unique<VulkanTransmissionImage>(m_device->GetPhysicalDevice(), m_device->GetHandle());

    m_atmosphere = std::make_unique<VulkanAtmosphere>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        m_frameSetLayout->GetHandle());
    // The scene as the DDGI probe rays trace it, one instance buffer per frame in flight.
    m_rayScene = std::make_unique<VulkanRayScene>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    m_ddgi = std::make_unique<VulkanDdgi>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        m_frameSetLayout->GetHandle(),
        m_rayScene->GetSetLayout(),
        static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    m_environmentProbe = std::make_unique<VulkanEnvironmentProbe>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        m_frameSetLayout->GetHandle());

    // Set 0 binding 6 must name a valid image even when no HDRI is loaded.
    VulkanUploadBatch uploadBatch(
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        m_device->GetGraphicsQueue());
    FloatTextureData black{};
    black.width = 1;
    black.height = 1;
    black.pixels = {0.0f, 0.0f, 0.0f, 1.0f};
    m_defaultEnvironmentMap = std::make_unique<VulkanTexture>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        black,
        uploadBatch);
    // The DFG table, one mip, RGBA32F; the shader clamps its lookups to texel centres, so the
    // equirectangular sampler's u repeat never shows.
    m_environmentBrdfLut = std::make_unique<VulkanTexture>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        BuildEnvironmentBrdfLut(kEnvironmentBrdfLutSize, kEnvironmentBrdfSampleCount),
        uploadBatch);
    // The LTC tables; like the DFG table, the shader clamps its lookups to texel centres.
    const auto ltcTexture = [](const std::array<float, kLtcTableSize * kLtcTableSize * 4>& table)
    {
        FloatTextureData data{};
        data.width = static_cast<int>(kLtcTableSize);
        data.height = static_cast<int>(kLtcTableSize);
        data.pixels.assign(table.begin(), table.end());
        return data;
    };
    m_ltcInverseMatrices = std::make_unique<VulkanTexture>(
        m_device->GetPhysicalDevice(), m_device->GetHandle(), ltcTexture(kLtcInverseMatrices), uploadBatch);
    m_ltcAmplitudes = std::make_unique<VulkanTexture>(
        m_device->GetPhysicalDevice(), m_device->GetHandle(), ltcTexture(kLtcAmplitudes), uploadBatch);
    uploadBatch.Flush();

    m_gpuTimer = std::make_unique<VulkanGpuTimer>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
}

void VulkanRenderer::LogFrameTimings() const
{
    const auto average = [](const std::vector<double>& samples)
    {
        double sum = 0.0;
        for (double sample : samples)
        {
            sum += sample;
        }
        return samples.empty() ? 0.0 : sum / static_cast<double>(samples.size());
    };
    LOG_INFO(
        "Frame timings over the last {} frames: CPU {:.2f} ms recording, {:.2f} ms waiting on the GPU; GPU {:.2f} ms; "
        "slowest frame {:.1f} ms of CPU",
        m_cpuFrameMs.size(),
        average(m_cpuFrameMs),
        average(m_cpuWaitMs),
        m_gpuTimer ? m_gpuTimer->GetAverageFrameMs() : 0.0,
        m_cpuFrameMs.empty() ? 0.0 : *std::max_element(m_cpuFrameMs.begin(), m_cpuFrameMs.end()));
    for (const CpuStageTimer::Stage& stage : m_cpuStages.GetStages())
    {
        LOG_INFO("  CPU {:<20} {:7.3f} ms", stage.name, stage.averageMs);
    }
    if (m_gpuTimer)
    {
        for (const VulkanGpuTimer::Section& section : m_gpuTimer->GetSections())
        {
            LOG_INFO("  GPU {:<20} {:7.3f} ms", section.name, section.averageMs);
        }
    }
}

void VulkanRenderer::CaptureViewport(const std::filesystem::path& path)
{
    if (!m_lastRecordedImageIndex.has_value() || !m_sceneTargets)
    {
        throw std::runtime_error("No frame has been drawn to capture");
    }
    vkDeviceWaitIdle(m_device->GetHandle());

    ImageCaptureRequest request{};
    request.physicalDevice = m_device->GetPhysicalDevice();
    request.device = m_device->GetHandle();
    request.queueFamily = m_device->GetQueueFamilies().graphicsFamily.value();
    request.queue = m_device->GetGraphicsQueue();
    // SceneLdr is indexed by swapchain image; the frame slot is ignored for it.
    const uint32_t index = m_sceneTargets->ResolveIndex(RenderTargetId::SceneLdr, *m_lastRecordedImageIndex, 0);
    request.image = m_sceneTargets->GetImage(RenderTargetId::SceneLdr, index);
    request.format = m_sceneTargets->GetFormat(RenderTargetId::SceneLdr);
    request.extent = m_sceneTargets->GetExtent();
    // The ImGui pass sampled it last, so the tracker left it shader-read.
    request.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CaptureImageToPng(request, path);
    LOG_INFO("Captured the viewport to '{}' at EV100 {:.2f}", path.string(), State().camera.exposureEv100);
}

void VulkanRenderer::SubmitVideoFrame(uint32_t frameSlot)
{
    if (!m_videoReadback)
    {
        return;
    }
    std::optional<VulkanVideoReadback::Frame> frame = m_videoReadback->Take(frameSlot);
    VideoRecorder* const recorder = ActiveVideoRecorder();
    if (frame && recorder != nullptr && frame->extent.width == recorder->GetSettings().width &&
        frame->extent.height == recorder->GetSettings().height)
    {
        recorder->Submit(std::move(frame->frame));
    }
}

void VulkanRenderer::FlushVideoFrames()
{
    if (!m_videoReadback || !m_videoReadback->HasPending())
    {
        return;
    }
    vkDeviceWaitIdle(m_device->GetHandle());
    // Oldest first: the recorder writes them in the order it is given them.
    std::vector<VulkanVideoReadback::Frame> frames;
    for (uint32_t slot = 0; slot < VulkanCommandContext::kMaxFramesInFlight; ++slot)
    {
        if (std::optional<VulkanVideoReadback::Frame> frame = m_videoReadback->Take(slot))
        {
            frames.push_back(std::move(*frame));
        }
    }
    std::sort(frames.begin(), frames.end(), [](const auto& left, const auto& right)
              {
                  return left.frame.timeSeconds < right.frame.timeSeconds;
              });
    VideoRecorder* const recorder = ActiveVideoRecorder();
    for (VulkanVideoReadback::Frame& frame : frames)
    {
        if (recorder != nullptr && frame.extent.width == recorder->GetSettings().width &&
            frame.extent.height == recorder->GetSettings().height)
        {
            recorder->Submit(std::move(frame.frame));
        }
    }
}

void VulkanRenderer::DestroyDeviceResources()
{
    m_gpuTimer.reset();
    m_environmentMap.reset();
    m_environmentMapPath.clear();
    m_defaultEnvironmentMap.reset();
    m_environmentBrdfLut.reset();
    m_ltcInverseMatrices.reset();
    m_ltcAmplitudes.reset();
    m_environmentProbe.reset();
    m_ddgi.reset();
    m_rayScene.reset();
    m_atmosphere.reset();
    // Its pipelines were built against the material set layout released below.
    m_shadowPass.reset();
    m_localShadowPass.reset();
    m_transmissionImage.reset();
    if (m_pipelineCache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(m_device->GetHandle(), m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
    }
    // The cached material sets come from pools made against the layout; content that named them is gone.
    m_renderSubmeshes.clear();
    m_liveSubmeshes.clear();
    m_materialSets.reset();
    m_materialSetLayout.reset();
    m_frameSetLayout.reset();
}

EnvironmentDescriptorBindings VulkanRenderer::BuildEnvironmentBindings() const
{
    EnvironmentDescriptorBindings bindings{};
    bindings.transmittance = m_atmosphere->GetTransmittanceBinding();
    bindings.skyView = m_atmosphere->GetSkyViewBinding();
    bindings.aerialPerspective = m_atmosphere->GetAerialPerspectiveBinding();
    bindings.cloudShapeNoise = m_atmosphere->GetCloudShapeNoiseBinding();
    bindings.cloudDetailNoise = m_atmosphere->GetCloudDetailNoiseBinding();
    bindings.cloudShadow = m_atmosphere->GetCloudShadowBinding();
    bindings.irradiance = m_atmosphere->GetIrradianceBuffer();
    bindings.prefiltered = m_environmentProbe->GetPrefilteredBinding();
    bindings.brdfLut = TextureDescriptorBinding{m_environmentBrdfLut->GetImageView(), m_environmentBrdfLut->GetSampler()};
    bindings.ltcInverseMatrices = TextureDescriptorBinding{m_ltcInverseMatrices->GetImageView(), m_ltcInverseMatrices->GetSampler()};
    bindings.ltcAmplitudes = TextureDescriptorBinding{m_ltcAmplitudes->GetImageView(), m_ltcAmplitudes->GetSampler()};
    bindings.transmission = m_transmissionImage->GetSampledBinding();
    bindings.scatterLight = m_scatterPass->GetLightBinding();
    bindings.scatterDepth = m_scatterPass->GetDepthBinding();
    bindings.ddgiIrradiance = m_ddgi->GetIrradianceBinding();
    bindings.ddgiVisibility = m_ddgi->GetVisibilityBinding();
    bindings.ddgiProbeStates = m_ddgi->GetProbeStateBuffer();
    const VulkanTexture& environmentMap = m_environmentMap ? *m_environmentMap : *m_defaultEnvironmentMap;
    bindings.environmentMap = TextureDescriptorBinding{environmentMap.GetImageView(), environmentMap.GetSampler()};
    return bindings;
}

// The mode the frame renders with: an HDRI that is still loading, or failed to, renders as None.
EnvironmentMode VulkanRenderer::EffectiveEnvironmentMode(const SceneEnvironment& environment) const
{
    if (environment.mode != EnvironmentMode::Hdri)
    {
        return environment.mode;
    }
    const bool loaded = m_environmentMap && !environment.hdri.path.empty() && environment.hdri.path == m_environmentMapPath;
    return loaded ? EnvironmentMode::Hdri : EnvironmentMode::None;
}

void VulkanRenderer::UpdateMinimapTexture()
{
    const SceneMinimap& minimap = EditorWorld().GetMinimap();
    const std::string path = minimap.IsValid() ? minimap.image : std::string{};
    if (path != m_minimapPath)
    {
        ReleaseMinimapTexture();
        m_minimapPath = path;
        if (!path.empty())
        {
            std::filesystem::path file(path);
            std::error_code ec;
            if (file.is_relative() && !std::filesystem::exists(file, ec))
            {
                file = EnginePaths::ProjectRoot() / file;
            }
            try
            {
                VulkanUploadBatch uploadBatch(
                    m_device->GetHandle(),
                    m_device->GetQueueFamilies().graphicsFamily.value(),
                    m_device->GetGraphicsQueue());
                m_minimapTexture = std::make_unique<VulkanTexture>(
                    m_device->GetPhysicalDevice(),
                    m_device->GetHandle(),
                    file.string(),
                    uploadBatch);
                uploadBatch.Flush();
                // Clamped, not repeated: past the picture's edges the minimap shows the edge's colour
                // (the sea, on a game's radar map) rather than the far side of the map.
                TextureSampler sampler;
                sampler.wrapS = TextureWrap::ClampToEdge;
                sampler.wrapT = TextureWrap::ClampToEdge;
                m_minimapBinding = ImGui_ImplVulkan_AddTexture(
                    m_samplerCache->Get(sampler),
                    m_minimapTexture->GetImageView(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                LOG_INFO("Minimap: loaded '{}'", file.string());
            }
            catch (const std::exception& error)
            {
                LOG_WARN("Minimap: could not load '{}': {}", file.string(), error.what());
                m_minimapTexture.reset();
            }
        }
    }
    State().editorUi.SetMinimapTexture(
        m_minimapBinding != VK_NULL_HANDLE ? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(m_minimapBinding)) : ImTextureID{});
}

void VulkanRenderer::ReleaseMinimapTexture()
{
    if (m_minimapBinding == VK_NULL_HANDLE && m_minimapTexture == nullptr)
    {
        return;
    }
    // Frames in flight may still sample it; a scene change is rare enough to wait for them.
    vkDeviceWaitIdle(m_device->GetHandle());
    if (m_minimapBinding != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_RemoveTexture(m_minimapBinding);
        m_minimapBinding = VK_NULL_HANDLE;
    }
    m_minimapTexture.reset();
}

void VulkanRenderer::UpdateEnvironmentMap(const SceneEnvironment& environment)
{
    if (environment.mode != EnvironmentMode::Hdri || environment.hdri.path.empty())
    {
        return;
    }
    const std::string& wanted = environment.hdri.path;

    if (m_pendingEnvironmentMap.valid())
    {
        if (m_pendingEnvironmentMap.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }
        const std::string path = m_pendingEnvironmentMapPath;
        try
        {
            PreparedEnvironmentMap prepared = m_pendingEnvironmentMap.get();
            const FloatTextureData& image = prepared.image;
            if (path == wanted)
            {
                VulkanUploadBatch uploadBatch(
                    m_device->GetHandle(),
                    m_device->GetQueueFamilies().graphicsFamily.value(),
                    m_device->GetGraphicsQueue());
                auto texture = std::make_unique<VulkanTexture>(
                    m_device->GetPhysicalDevice(), m_device->GetHandle(), image, uploadBatch);
                uploadBatch.Flush();
                // The frame sets name the old map until rewritten, and may be in use.
                m_commandContext->WaitForAllFrames();
                if (m_uniformBuffer)
                {
                    m_uniformBuffer->SetEnvironmentMap(TextureDescriptorBinding{texture->GetImageView(), texture->GetSampler()});
                }
                if (m_environmentProbe)
                {
                    // A new image behind the same parameters: the cached capture shows the old one.
                    m_environmentProbe->Invalidate();
                }
                m_environmentMap = std::move(texture);
                m_environmentMapPath = path;
                m_environmentMapSh = prepared.sh;
                LOG_INFO("Loaded HDRI '{}' ({}x{})", path, image.width, image.height);
            }
        }
        catch (const std::exception& error)
        {
            LOG_ERROR("Failed to load HDRI '{}': {}", path, error.what());
            m_failedEnvironmentMapPath = path;
        }
        m_pendingEnvironmentMapPath.clear();
    }

    if (wanted == m_environmentMapPath || wanted == m_failedEnvironmentMapPath)
    {
        return;
    }
    m_pendingEnvironmentMapPath = wanted;
    m_pendingEnvironmentMap = std::async(
        std::launch::async,
        [path = wanted]()
        {
            PreparedEnvironmentMap prepared{};
            prepared.image = TextureLoader::LoadRGBA32F(path);
            prepared.sh = ProjectEquirectangular(prepared.image);
            return prepared;
        });
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
    m_scatterPass = nullptr;
    m_forwardPipelines.reset();
    m_scatterPipelines.reset();
    m_geometryPipelines.reset();
    m_decalPipelines.reset();
    m_gbufferDescriptors = std::make_unique<VulkanGBufferDescriptors>(m_device->GetHandle(), *m_sceneTargets);

    auto geometryPass = std::make_unique<VulkanGeometryPass>(
        m_device->GetHandle(), m_pipelineCache, *m_sceneTargets, m_frameSetLayout->GetHandle());
    auto forwardPass = std::make_unique<VulkanForwardPass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle(),
        ForwardPassPart::OpaqueAndSky);

    MaterialPipelineSetConfig geometryConfig{};
    geometryConfig.fragmentShader = "gbuffer.frag.spv";
    geometryConfig.colorAttachmentCount = VulkanGeometryPass::kColorAttachmentCount;
    geometryConfig.writeAlpha = true;
    geometryConfig.allowBlending = false;
    geometryConfig.depthLessOrEqual = false;

    // Both sets are built here, while the typed pass pointers are in hand: the pipelines depend on
    // nothing but these render passes and the two device lifetime set layouts.
    m_geometryPipelines = std::make_unique<VulkanPipelineSet>(
        m_device->GetHandle(),
        m_pipelineCache,
        geometryPass->GetRenderPass(),
        m_frameSetLayout->GetHandle(),
        m_materialSetLayout->GetHandle(),
        geometryConfig);
    if (m_device->SupportsIndependentBlend())
    {
        MaterialPipelineSetConfig decalConfig = geometryConfig;
        decalConfig.allowBlending = true;
        decalConfig.depthLessOrEqual = true;
        decalConfig.decal = true;
        m_decalPipelines = std::make_unique<VulkanPipelineSet>(
            m_device->GetHandle(),
            m_pipelineCache,
            geometryPass->GetRenderPass(),
            m_frameSetLayout->GetHandle(),
            m_materialSetLayout->GetHandle(),
            decalConfig);
    }
    // The default config is the forward shape: triangle.frag, one HDR attachment, RGB writes.
    m_forwardPipelines = std::make_unique<VulkanPipelineSet>(
        m_device->GetHandle(),
        m_pipelineCache,
        forwardPass->GetRenderPass(),
        m_frameSetLayout->GetHandle(),
        m_materialSetLayout->GetHandle(),
        MaterialPipelineSetConfig{});
    // The scatter pre-pass: the forward shader's inputs, its own output (light and draw slot, alpha
    // included), its own depth, no blending.
    auto scatterPass = std::make_unique<VulkanScatterPass>(m_device->GetPhysicalDevice(), m_device->GetHandle(), *m_sceneTargets);
    MaterialPipelineSetConfig scatterConfig{};
    scatterConfig.writeAlpha = true;
    scatterConfig.allowBlending = false;
    scatterConfig.depthLessOrEqual = false;
    scatterConfig.scatterPrepass = true;
    m_scatterPipelines = std::make_unique<VulkanPipelineSet>(
        m_device->GetHandle(),
        m_pipelineCache,
        scatterPass->GetRenderPass(),
        m_frameSetLayout->GetHandle(),
        m_materialSetLayout->GetHandle(),
        scatterConfig);
    m_scatterPass = scatterPass.get();

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
    m_scenePasses.push_back(std::make_unique<VulkanAoTracePass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle()));
    m_scenePasses.push_back(std::make_unique<VulkanAoResolvePass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle()));
    m_scenePasses.push_back(std::make_unique<VulkanLightingPass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle(),
        m_gbufferDescriptors->GetEmptySetLayout(),
        m_gbufferDescriptors->GetSetLayout()));
    m_scenePasses.push_back(std::make_unique<VulkanGiTracePass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle()));
    m_scenePasses.push_back(std::make_unique<VulkanGiResolvePass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle()));
    m_scenePasses.push_back(std::make_unique<VulkanGiCompositePass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle(),
        m_gbufferDescriptors->GetEmptySetLayout(),
        m_gbufferDescriptors->GetSetLayout()));
    m_scenePasses.push_back(std::make_unique<VulkanDdgiDebugPass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle(),
        *m_rayScene));
    m_scenePasses.push_back(std::move(scatterPass));
    m_scenePasses.push_back(std::move(forwardPass));
    m_scenePasses.push_back(std::make_unique<VulkanTransmissionCopyPass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle(),
        *m_transmissionImage));
    // Compatible with the opaque half's render pass, so the same forward pipelines draw in it.
    m_scenePasses.push_back(std::make_unique<VulkanForwardPass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle(),
        ForwardPassPart::Translucent));
    auto taaPass = std::make_unique<VulkanTaaPass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle());
    const VulkanTaaPass& taa = *taaPass;
    m_scenePasses.push_back(std::move(taaPass));
    // After TAA in this list, whose order OnTargetsRebuilt follows: the trace names TAA's history
    // images, which TAA recreates first.
    m_scenePasses.push_back(std::make_unique<VulkanSsrTracePass>(
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle(),
        taa));
    m_scenePasses.push_back(std::make_unique<VulkanSsrResolvePass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle()));
    m_scenePasses.push_back(std::make_unique<VulkanBloomPass>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        *m_sceneTargets,
        m_frameSetLayout->GetHandle()));
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
    m_uniformBuffer = std::make_unique<VulkanUniformBuffer>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        static_cast<uint32_t>(m_swapchain->GetImageViews().size()),
        m_frameSetLayout->GetHandle(),
        m_shadowPass->GetSampledBinding(),
        m_localShadowPass->GetSampledBinding(),
        BuildEnvironmentBindings(),
        m_drawSlotWatermark);
    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
    {
        m_uniformBuffer->WriteDrawSlot(renderSubmesh->drawSlot, renderSubmesh->material, renderSubmesh->textureTransforms);
    }
}

uint32_t VulkanRenderer::AcquireDrawSlot()
{
    if (!m_freeDrawSlots.empty())
    {
        const uint32_t slot = m_freeDrawSlots.back();
        m_freeDrawSlots.pop_back();
        return slot;
    }
    return m_drawSlotWatermark++;
}

void VulkanRenderer::ReleaseDrawSlot(uint32_t slot)
{
    if (slot != RenderSubmesh::kNoDrawSlot)
    {
        m_freeDrawSlots.push_back(slot);
    }
}

void VulkanRenderer::DestroyDescriptorResources()
{
    m_uniformBuffer.reset();
    // m_textureStore survives this teardown: the next upload reuses every live texture.
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

bool VulkanRenderer::SwapchainNeedsResize() const
{
    const VkExtent2D wanted = VulkanSwapchain::ChooseExtent(
        GetWindow().GetSDLWindow(),
        m_device->QuerySurfaceCapabilities());
    const VkExtent2D current = m_swapchain->GetExtent();
    return wanted.width != current.width || wanted.height != current.height;
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
    // The scatter pass recreated its images at the new extent; set 0 still names the old ones.
    if (m_uniformBuffer)
    {
        m_uniformBuffer->SetScatterImages(m_scatterPass->GetLightBinding(), m_scatterPass->GetDepthBinding());
    }
    m_layoutTracker.Reset();
    m_motionHistory.Reset();
    m_aoHistory.Reset();
    m_giHistory.Reset();
    m_ssrHistory.Reset();
    m_taaHistory.Reset();
    LOG_INFO(
        "Scene render targets resized to {}x{}",
        m_sceneTargets->GetExtent().width,
        m_sceneTargets->GetExtent().height);
}

void VulkanRenderer::UploadSceneResources()
{
    const auto uploadStart = std::chrono::steady_clock::now();
    // Textures stay in m_textureStore by cache key; a submesh the GPU already has keeps its own, so
    // only new submeshes look textures up. A throw anywhere below (running out of GPU memory,
    // typically) drops what this upload added and leaves every live texture where the content's
    // material sets expect it.
    std::vector<std::shared_ptr<const RenderSubmesh>> newRenderSubmeshes;
    // The cache keys of the textures the submesh being made uses, for the next upload that keeps it.
    std::vector<std::string>* recordedTextureKeys = nullptr;
    // The textures the new submeshes' material sets are written from (the defaults first), by index.
    std::vector<const VulkanTexture*> textureViews;
    std::unordered_map<std::string, uint32_t> keyToIndex;
    std::vector<std::string> addedTextureKeys;

    // Batch every texture and submesh-buffer upload below into a handful of submit+wait
    // rounds instead of one per resource: VulkanTexture/VulkanBuffer used to each own their
    // upload (command pool, submit, vkQueueWaitIdle), which serializes hundreds of GPU
    // round-trips in a row for models with many submeshes/textures (e.g. Sponza: 405 submeshes,
    // up to ~170 unique textures). Flushing periodically bounds how much staging memory is held
    // at once while still cutting the number of GPU stalls by roughly two orders of magnitude. The
    // batch stages out of its own few chunks, and flushes once this much is staged: a count of
    // resources flushed a streamed cell's thousand small buffers sixteen times, each a queue wait.
    constexpr VkDeviceSize kStagedBytesPerUploadFlush = VkDeviceSize{64} << 20;
    VulkanUploadBatch uploadBatch(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        m_device->GetGraphicsQueue());
    auto flushUploadBatchIfNeeded = [&]()
    {
        if (uploadBatch.StagedBytes() >= kStagedBytesPerUploadFlush)
        {
            uploadBatch.Flush();
        }
    };

    // Material texture files are uploaded block-compressed whenever the device allows it.
    const bool compressTextures = m_device->SupportsBlockCompression();

    auto indexOf = [&](const std::string& key, const VulkanTexture* texture) -> uint32_t
    {
        const uint32_t index = static_cast<uint32_t>(textureViews.size());
        textureViews.push_back(texture);
        keyToIndex.emplace(key, index);
        return index;
    };
    auto store = [&](const std::string& key, std::unique_ptr<VulkanTexture> texture, bool permanent) -> const VulkanTexture*
    {
        StoredTexture& entry = m_textureStore[key];
        entry.texture = std::move(texture);
        entry.permanent = permanent;
        addedTextureKeys.push_back(key);
        return entry.texture.get();
    };

    // A built-in texture by cache key: the stored one, else made now and kept for good.
    auto acquireDefault = [&](const std::string& id, const TextureData& data, VulkanTextureFormat fmt) -> uint32_t
    {
        const std::string key = id + (fmt == VulkanTextureFormat::SrgbColor ? "|srgb" : "|linear");
        if (auto it = keyToIndex.find(key); it != keyToIndex.end())
            return it->second;
        if (auto stored = m_textureStore.find(key); stored != m_textureStore.end())
            return indexOf(key, stored->second.texture.get());
        auto texture = std::make_unique<VulkanTexture>(m_device->GetPhysicalDevice(), m_device->GetHandle(), data, uploadBatch, fmt);
        flushUploadBatchIfNeeded();
        return indexOf(key, store(key, std::move(texture), true));
    };

    auto loadTextureIndex = [&](const std::string& texturePath, TextureUsage usage, uint32_t fallbackIndex) -> uint32_t
    {
        if (texturePath.empty())
            return fallbackIndex;

        const std::string key = BuildTextureCacheKey(texturePath, usage);
        // The workers could not decode it and logged why; the default stands in, as it always has.
        if (m_failedTextureKeys.count(key) != 0)
            return fallbackIndex;
        if (auto it = keyToIndex.find(key); it != keyToIndex.end())
        {
            if (recordedTextureKeys != nullptr)
                recordedTextureKeys->push_back(key);
            return it->second;
        }

        // Stored: on the GPU already, for another submesh.
        if (auto stored = m_textureStore.find(key); stored != m_textureStore.end())
        {
            if (recordedTextureKeys != nullptr)
                recordedTextureKeys->push_back(key);
            return indexOf(key, stored->second.texture.get());
        }

        // Staged: prepared by the workers and already uploaded, waiting for this commit.
        if (auto stagedIt = m_stagedTextures.find(key); stagedIt != m_stagedTextures.end())
        {
            std::unique_ptr<VulkanTexture> texture = std::move(stagedIt->second);
            m_stagedTextures.erase(stagedIt);
            if (recordedTextureKeys != nullptr)
                recordedTextureKeys->push_back(key);
            return indexOf(key, store(key, std::move(texture), false));
        }

        // Miss: prepare and upload here. Only the startup upload, or a texture the preparation
        // queue somehow never saw, gets this far, so correctness never depends on the queue. A
        // texture that cannot be decoded falls back to the default, but running out of memory fails
        // the whole upload: substituting white for a texture the GPU had no room for would hide
        // the problem and still leave no room for the geometry that follows.
        try
        {
            std::unique_ptr<VulkanTexture> texture = UploadPreparedTexture(
                PrepareTexture(texturePath, usage, compressTextures, TextureCacheDirectory()),
                usage,
                uploadBatch);
            flushUploadBatchIfNeeded();
            if (recordedTextureKeys != nullptr)
                recordedTextureKeys->push_back(key);
            return indexOf(key, store(key, std::move(texture), false));
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

    // Everything this upload made goes again if it fails: its material sets, and the textures it
    // stored, which no live submesh names yet.
    const auto dropAdded = [&]()
    {
        recordedTextureKeys = nullptr;
        m_materialSets->AbandonPending();
        for (const std::string& key : addedTextureKeys)
        {
            if (auto entry = m_textureStore.find(key); entry != m_textureStore.end() && entry->second.references == 0)
            {
                m_textureStore.erase(entry);
            }
        }
    };

    std::vector<std::shared_ptr<RenderSubmesh>> madeSubmeshes;
    std::vector<MaterialTextureSlots> madeSlots;
    std::unordered_map<entt::entity, uint32_t> nextSubmeshOrdinal;
    size_t newBufferCount = 0;
    size_t keptSubmeshCount = 0;
    try
    {
        const uint32_t defaultBaseColorIndex = acquireDefault("__default_base_color__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::SrgbColor);
        const uint32_t defaultNormalIndex = acquireDefault("__default_normal__", CreateFlatNormalTexture(), VulkanTextureFormat::LinearData);
        const uint32_t defaultMetallicIndex = acquireDefault("__default_metallic__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
        const uint32_t defaultRoughnessIndex = acquireDefault("__default_roughness__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
        const uint32_t defaultOcclusionIndex = acquireDefault("__default_occlusion__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
        const uint32_t defaultEmissiveIndex = acquireDefault("__default_emissive__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::SrgbColor);
        const uint32_t defaultBlendMaskIndex = acquireDefault("__default_blend_mask__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
        // The layer maps multiply their factors, so white leaves the factors alone; the anisotropy map
        // is a direction, and (1, 0.5) is +X, the tangent, at full strength.
        const uint32_t defaultLayerIndex = acquireDefault("__default_layer__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::LinearData);
        // White in sRGB, for the colour maps among the layers (sheen colour, specular colour).
        const uint32_t defaultSheenColorIndex = acquireDefault("__default_sheen_color__", CreateSolidTexture(255, 255, 255, 255), VulkanTextureFormat::SrgbColor);
        const uint32_t defaultAnisotropyIndex = acquireDefault("__default_anisotropy__", CreateSolidTexture(255, 128, 255, 255), VulkanTextureFormat::LinearData);

        const MaterialTextureSlots defaultSlots{
            defaultBaseColorIndex, defaultNormalIndex, defaultMetallicIndex, defaultRoughnessIndex,
            defaultOcclusionIndex, defaultEmissiveIndex,
            defaultBaseColorIndex, defaultNormalIndex, defaultMetallicIndex, defaultRoughnessIndex,
            defaultOcclusionIndex, defaultEmissiveIndex, defaultBlendMaskIndex,
            defaultLayerIndex, defaultLayerIndex, defaultSheenColorIndex, defaultLayerIndex, defaultAnisotropyIndex,
            defaultLayerIndex, defaultSheenColorIndex, defaultNormalIndex,
            defaultLayerIndex, defaultLayerIndex,
            defaultLayerIndex, defaultLayerIndex};

        // The geometry already on the GPU, by the CPU mesh it came from: a mesh the model cache still
        // holds is the same data, so its buffers carry over. Gathered on the first new submesh.
        std::unordered_map<const MeshData*, std::shared_ptr<VulkanBuffer>> liveBuffers;
        bool liveBuffersGathered = false;
        for (const CpuRenderSubmesh& cpuRenderSubmesh : State().rendererWorld.GetRenderSubmeshes())
        {
            const uint32_t ordinal = nextSubmeshOrdinal[cpuRenderSubmesh.entity]++;

            // Kept: a submesh the GPU already has, with its textures, material set and draw slot.
            if (const auto live = m_liveSubmeshes.find(cpuRenderSubmesh.revision); live != m_liveSubmeshes.end())
            {
                newRenderSubmeshes.push_back(live->second);
                ++keptSubmeshCount;
                continue;
            }

            if (!liveBuffersGathered)
            {
                liveBuffers.reserve(m_renderSubmeshes.size());
                for (const std::shared_ptr<const RenderSubmesh>& live : m_renderSubmeshes)
                {
                    if (live->mesh && live->buffer)
                    {
                        liveBuffers.emplace(live->mesh.get(), live->buffer);
                    }
                }
                liveBuffersGathered = true;
            }

            auto renderSubmesh = std::make_shared<RenderSubmesh>();
            renderSubmesh->entity = cpuRenderSubmesh.entity;
            renderSubmesh->revision = cpuRenderSubmesh.revision;
            renderSubmesh->motionKey = MotionKey{static_cast<uint32_t>(entt::to_integral(cpuRenderSubmesh.entity)), ordinal};
            renderSubmesh->mesh = cpuRenderSubmesh.mesh;
            if (const auto live = liveBuffers.find(cpuRenderSubmesh.mesh.get()); live != liveBuffers.end())
            {
                renderSubmesh->buffer = live->second;
            }
            else
            {
                renderSubmesh->buffer = std::make_shared<VulkanBuffer>(
                    m_device->GetPhysicalDevice(), m_device->GetHandle(),
                    *cpuRenderSubmesh.mesh, uploadBatch);
                liveBuffers.emplace(cpuRenderSubmesh.mesh.get(), renderSubmesh->buffer);
                ++newBufferCount;
                flushUploadBatchIfNeeded();
            }
            renderSubmesh->material = cpuRenderSubmesh.material;
            renderSubmesh->textureTransforms = BuildGpuTextureTransforms(cpuRenderSubmesh.textureTransforms);
            renderSubmesh->doubleSided = cpuRenderSubmesh.doubleSided;
            renderSubmesh->alphaMode = cpuRenderSubmesh.alphaMode;
            renderSubmesh->decal = cpuRenderSubmesh.decal;
            renderSubmesh->localBoundsCenter = cpuRenderSubmesh.localBoundsCenter;
            renderSubmesh->localBoundsRadius = cpuRenderSubmesh.localBoundsRadius;
            renderSubmesh->name = cpuRenderSubmesh.name;

            MaterialTextureSlots slots = defaultSlots;
            if (cpuRenderSubmesh.hasTexCoords)
            {
                recordedTextureKeys = &renderSubmesh->textureKeys;
                slots.samplers = cpuRenderSubmesh.textureSamplers;
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
                slots.clearcoat = loadTextureIndex(cpuRenderSubmesh.textures.clearcoat, TextureUsage::Data, defaultLayerIndex);
                slots.clearcoatRoughness = loadTextureIndex(cpuRenderSubmesh.textures.clearcoatRoughness, TextureUsage::Data, defaultLayerIndex);
                slots.sheenColor = loadTextureIndex(cpuRenderSubmesh.textures.sheenColor, TextureUsage::Color, defaultSheenColorIndex);
                slots.sheenRoughness = loadTextureIndex(cpuRenderSubmesh.textures.sheenRoughness, TextureUsage::Data, defaultLayerIndex);
                slots.anisotropy = loadTextureIndex(cpuRenderSubmesh.textures.anisotropy, TextureUsage::Data, defaultAnisotropyIndex);
                slots.specular = loadTextureIndex(cpuRenderSubmesh.textures.specular, TextureUsage::Data, defaultLayerIndex);
                slots.specularColor = loadTextureIndex(cpuRenderSubmesh.textures.specularColor, TextureUsage::Color, defaultSheenColorIndex);
                slots.clearcoatNormal = loadTextureIndex(cpuRenderSubmesh.textures.clearcoatNormal, TextureUsage::Normal, defaultNormalIndex);
                slots.iridescence = loadTextureIndex(cpuRenderSubmesh.textures.iridescence, TextureUsage::Data, defaultLayerIndex);
                slots.iridescenceThickness =
                    loadTextureIndex(cpuRenderSubmesh.textures.iridescenceThickness, TextureUsage::Data, defaultLayerIndex);
                slots.transmission = loadTextureIndex(cpuRenderSubmesh.textures.transmission, TextureUsage::Data, defaultLayerIndex);
                slots.thickness = loadTextureIndex(cpuRenderSubmesh.textures.thickness, TextureUsage::Data, defaultLayerIndex);
                slots.diffuseTransmission = loadTextureIndex(cpuRenderSubmesh.textures.diffuseTransmission, TextureUsage::Data, defaultLayerIndex);
                slots.diffuseTransmissionColor =
                    loadTextureIndex(cpuRenderSubmesh.textures.diffuseTransmissionColor, TextureUsage::Color, defaultSheenColorIndex);
                slots.detailMask = loadTextureIndex(cpuRenderSubmesh.textures.detailMask, TextureUsage::Data, defaultLayerIndex);
                for (size_t layer = 0; layer < kDetailLayerCount; ++layer)
                {
                    slots.detailLayers[layer] = loadTextureIndex(cpuRenderSubmesh.textures.detailLayers[layer], TextureUsage::Data, defaultLayerIndex);
                }
                recordedTextureKeys = nullptr;
            }
            // One reference per texture, however many slots name it.
            std::sort(renderSubmesh->textureKeys.begin(), renderSubmesh->textureKeys.end());
            renderSubmesh->textureKeys.erase(
                std::unique(renderSubmesh->textureKeys.begin(), renderSubmesh->textureKeys.end()), renderSubmesh->textureKeys.end());
            madeSubmeshes.push_back(renderSubmesh);
            madeSlots.push_back(slots);
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
        }

        // The new submeshes' material sets: kept where another submesh already has the same textures.
        const std::vector<MaterialTextureBinding> bindings = BuildMaterialTextureBindings(textureViews, madeSlots, *m_samplerCache);
        for (size_t index = 0; index < madeSubmeshes.size(); ++index)
        {
            madeSubmeshes[index]->materialSet = m_materialSets->Acquire(bindings[index]);
            madeSubmeshes[index]->rayBaseColor = bindings[index].baseColor;
            madeSubmeshes[index]->rayEmissive = bindings[index].emissive;
        }
        uploadBatch.Flush();
    }
    catch (...)
    {
        dropAdded();
        throw;
    }
    const double uploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStart).count();
    const TextureUploadStats& stats = m_textureUploadStats;
    if (stats.fromCache + stats.compressedNow + stats.uncompressed + stats.floatTextures > 0)
    {
        LOG_INFO(
            "Texture files: {} block-compressed from the cache, {} compressed now ({:.1f} s of encoding across threads), {} uncompressed, {} float",
            stats.fromCache,
            stats.compressedNow,
            stats.compressSeconds,
            stats.uncompressed,
            stats.floatTextures);
    }

    const size_t submeshCount = newRenderSubmeshes.size();
    const auto applyStart = std::chrono::steady_clock::now();
    try
    {
        ApplyRenderContent(std::move(newRenderSubmeshes), keptSubmeshCount);
    }
    catch (...)
    {
        dropAdded();
        throw;
    }
    LOG_INFO(
        "Uploaded {} submeshes ({} kept, {} new buffers, {} textures stored) in {:.0f} ms, then {:.0f} ms for descriptors and the ray scene",
        submeshCount,
        keptSubmeshCount,
        newBufferCount,
        m_textureStore.size(),
        uploadMs,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count());
}

void VulkanRenderer::DropUnreferencedTextures()
{
    std::erase_if(m_textureStore, [](const auto& entry)
                  {
                      return entry.second.references == 0 && !entry.second.permanent;
                  });
}

void VulkanRenderer::UploadSceneResourcesOrKeepPrevious()
{
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
        AbandonPendingTextures();
        DropSubmeshesOfRemovedEntities();
        return;
    }

    // Staged textures the scene no longer needed are released with the change they were for.
    m_stagedTextures.clear();
    m_failedTextureKeys.clear();
    m_texturesRequested = 0;
    m_textureUploadStats = TextureUploadStats{};

    // The screen matches the scene again, so a report of it not matching is now stale.
    if (State().lastModelLoadError == kOutOfMemoryReport)
    {
        State().lastModelLoadError.clear();
    }
}

void VulkanRenderer::RequestSceneUpload()
{
    // The previous content stays on screen until the change commits, so it must stop drawing any
    // entity the change deleted right away.
    DropSubmeshesOfRemovedEntities();

    for (const CpuRenderSubmesh& submesh : State().rendererWorld.GetRenderSubmeshes())
    {
        // A submesh the GPU already draws has every texture it names.
        if (!submesh.hasTexCoords || m_liveSubmeshes.count(submesh.revision) != 0)
        {
            continue;
        }
        ForEachMaterialTexture(submesh, [&](const std::string& path, TextureUsage usage)
                               {
                                   if (path.empty())
                                   {
                                       return;
                                   }
                                   std::string key = BuildTextureCacheKey(path, usage);
                                   if (m_textureStore.count(key) != 0 || m_stagedTextures.count(key) != 0 ||
                                       m_failedTextureKeys.count(key) != 0)
                                   {
                                       return;
                                   }
                                   if (m_texturePreparation->Enqueue(TexturePreparationRequest{std::move(key), path, usage}))
                                   {
                                       ++m_texturesRequested;
                                   }
                               });
    }
    m_sceneUploadPending = true;
}

void VulkanRenderer::PumpSceneUpload()
{
    // A few per frame: each upload copies megabytes and waits for the queue, and the frame loop
    // should keep its pace while a large scene streams in.
    // Small map textures by the thousand (a streamed cell brings hundreds) are a memcpy into the batch's
    // staging and a pooled image each; four a frame held a cell back for seconds.
    constexpr size_t kStagedTexturesPerFrame = 64;
    std::vector<TexturePreparationResult> completed = m_texturePreparation->TakeCompleted(kStagedTexturesPerFrame);

    // Results of a change that was abandoned are dropped; a later change prepares what it needs
    // again, from the compressed texture cache.
    if (m_sceneUploadPending && !completed.empty())
    {
        try
        {
            VulkanUploadBatch uploadBatch(
                m_device->GetPhysicalDevice(),
                m_device->GetHandle(),
                m_device->GetQueueFamilies().graphicsFamily.value(),
                m_device->GetGraphicsQueue());
            for (TexturePreparationResult& result : completed)
            {
                if (!result.texture)
                {
                    LOG_ERROR("Failed to load model texture '{}': {}", result.key, result.error);
                    m_failedTextureKeys.insert(result.key);
                    continue;
                }
                m_stagedTextures[result.key] = UploadPreparedTexture(*result.texture, result.usage, uploadBatch);
            }
            uploadBatch.Flush();
        }
        catch (const std::exception& error)
        {
            if (!IsOutOfMemoryError(error))
            {
                throw;
            }
            LOG_ERROR("Keeping the previous scene content, staging a texture ran out of GPU memory: {}", error.what());
            State().lastModelLoadError = kOutOfMemoryReport;
            AbandonPendingTextures();
        }
    }

    m_cpuStages.Mark("StageTextures");
    if (m_sceneUploadPending && m_texturePreparation->IsIdle())
    {
        m_sceneUploadPending = false;
        UploadSceneResourcesOrKeepPrevious();
        m_cpuStages.Mark("CommitContent");
    }

    if (m_sceneUploadPending)
    {
        const size_t pending = m_texturePreparation->PendingCount();
        const size_t done = m_texturesRequested > pending ? m_texturesRequested - pending : 0;
        State().sceneUploadStatus =
            "Preparing textures: " + std::to_string(done) + " of " + std::to_string(m_texturesRequested);
    }
    else
    {
        State().sceneUploadStatus.clear();
    }
}

void VulkanRenderer::AbandonPendingTextures()
{
    // Staged textures were uploaded by batches that have completed and are referenced by no
    // descriptor set, so they can go at once.
    m_sceneUploadPending = false;
    m_stagedTextures.clear();
    m_failedTextureKeys.clear();
    m_texturesRequested = 0;
    m_textureUploadStats = TextureUploadStats{};
}

std::unique_ptr<VulkanTexture> VulkanRenderer::UploadPreparedTexture(
    const PreparedTexture& prepared,
    TextureUsage usage,
    VulkanUploadBatch& uploadBatch)
{
    TextureUploadStats& stats = m_textureUploadStats;
    if (prepared.halfFloat)
    {
        ++stats.floatTextures;
        return std::make_unique<VulkanTexture>(
            m_device->GetPhysicalDevice(), m_device->GetHandle(),
            *prepared.halfFloat, uploadBatch);
    }
    if (!prepared.compressed)
    {
        ++stats.uncompressed;
        return std::make_unique<VulkanTexture>(
            m_device->GetPhysicalDevice(), m_device->GetHandle(),
            prepared.rgba, uploadBatch, ToVulkanTextureFormat(usage));
    }
    if (prepared.fromCache)
    {
        ++stats.fromCache;
    }
    else
    {
        ++stats.compressedNow;
        stats.compressSeconds += prepared.compressSeconds;
    }
    return std::make_unique<VulkanTexture>(
        m_device->GetPhysicalDevice(), m_device->GetHandle(),
        *prepared.compressed, uploadBatch);
}

void VulkanRenderer::DropSubmeshesOfRemovedEntities()
{
    const ISceneWorld& sceneWorld = State().rendererWorld.GetSceneWorld();
    const auto isRemoved = [&sceneWorld](const std::shared_ptr<const RenderSubmesh>& renderSubmesh)
    {
        return !sceneWorld.IsValidEntity(renderSubmesh->entity);
    };
    if (std::none_of(m_renderSubmeshes.begin(), m_renderSubmeshes.end(), isRemoved))
    {
        return;
    }

    // The frames in flight may still draw from the buffers about to be destroyed. What remains is
    // a subset of the list the uniform buffer was sized for, so its motion slots still cover it;
    // material binding indices and motion keys are per submesh and stay valid.
    m_commandContext->WaitForAllFrames();
    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
    {
        if (isRemoved(renderSubmesh) && renderSubmesh->drawSlot != RenderSubmesh::kNoDrawSlot)
        {
            ReleaseDrawSlot(renderSubmesh->drawSlot);
            renderSubmesh->drawSlot = RenderSubmesh::kNoDrawSlot;
            for (const std::string& key : renderSubmesh->textureKeys)
            {
                if (auto entry = m_textureStore.find(key); entry != m_textureStore.end() && entry->second.references > 0)
                {
                    --entry->second.references;
                }
            }
            m_materialSets->Release(renderSubmesh->materialSet);
        }
    }
    std::erase_if(m_renderSubmeshes, isRemoved);
    std::erase_if(m_liveSubmeshes, [&isRemoved](const auto& entry)
                  {
                      return isRemoved(entry.second);
                  });
}

void VulkanRenderer::ApplyRenderContent(std::vector<std::shared_ptr<const RenderSubmesh>> newRenderSubmeshes, size_t keptSubmeshCount)
{
    // Up to the ray scene's content everything may throw and leaves the old content drawable;
    // afterwards nothing does. A change costs what it adds and drops: kept draws keep their slots,
    // textures, material sets and ray materials.
    if (m_swapchain && m_renderPass && !m_scenePasses.empty())
    {
        // Slots are written and descriptor sets freed below, which the frames in flight may still
        // read. The upload has just flushed and waited for its copies, so this costs next to nothing.
        m_commandContext->WaitForAllFrames();

        // Draws new to this content, and the old content's draws it drops (every kept one is in both).
        std::unordered_set<const RenderSubmesh*> kept;
        kept.reserve(keptSubmeshCount);
        std::vector<const RenderSubmesh*> placed;
        for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : newRenderSubmeshes)
        {
            if (renderSubmesh->drawSlot == RenderSubmesh::kNoDrawSlot)
            {
                placed.push_back(renderSubmesh.get());
            }
            else
            {
                kept.insert(renderSubmesh.get());
            }
        }
        std::vector<const RenderSubmesh*> dropped;
        std::vector<uint32_t> releasedSlots;
        for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
        {
            if (renderSubmesh->drawSlot != RenderSubmesh::kNoDrawSlot && kept.count(renderSubmesh.get()) == 0)
            {
                dropped.push_back(renderSubmesh.get());
                releasedSlots.push_back(renderSubmesh->drawSlot);
            }
        }

        // New draws take free slots, never one the old content still draws from: the dropped ones go
        // back only once this commit can no longer fail.
        for (const RenderSubmesh* renderSubmesh : placed)
        {
            renderSubmesh->drawSlot = AcquireDrawSlot();
        }
        try
        {
            if (!m_uniformBuffer || m_uniformBuffer->GetDrawCapacity() < m_drawSlotWatermark)
            {
                // More draws than the per-draw buffers hold: larger ones, with every draw of the old
                // content and the new written in, so either can be drawn from them.
                const uint32_t capacity = std::max({m_drawSlotWatermark, m_uniformBuffer ? m_uniformBuffer->GetDrawCapacity() * 3 / 2 : 0u, 256u});
                auto grown = std::make_unique<VulkanUniformBuffer>(
                    m_device->GetPhysicalDevice(),
                    m_device->GetHandle(),
                    static_cast<uint32_t>(m_swapchain->GetImageViews().size()),
                    m_frameSetLayout->GetHandle(),
                    m_shadowPass->GetSampledBinding(),
                    m_localShadowPass->GetSampledBinding(),
                    BuildEnvironmentBindings(),
                    capacity);
                for (const auto* list : {&m_renderSubmeshes, &newRenderSubmeshes})
                {
                    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : *list)
                    {
                        if (renderSubmesh->drawSlot != RenderSubmesh::kNoDrawSlot)
                        {
                            grown->WriteDrawSlot(renderSubmesh->drawSlot, renderSubmesh->material, renderSubmesh->textureTransforms);
                        }
                    }
                }
                m_uniformBuffer = std::move(grown);
            }
            else
            {
                // Free slots, which no draw of the old content reads.
                for (const RenderSubmesh* renderSubmesh : placed)
                {
                    m_uniformBuffer->WriteDrawSlot(renderSubmesh->drawSlot, renderSubmesh->material, renderSubmesh->textureTransforms);
                }
            }

            // The ray scene: every draw's mesh and slot, and the ray materials of the slots that change
            // hands.
            std::vector<RaySceneSubmesh> raySubmeshes;
            raySubmeshes.reserve(newRenderSubmeshes.size());
            for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : newRenderSubmeshes)
            {
                raySubmeshes.push_back(RaySceneSubmesh{renderSubmesh->mesh, renderSubmesh->alphaMode == MaterialAlphaMode::Blend, renderSubmesh->drawSlot});
            }
            std::vector<RayMaterialSource> placedMaterials;
            placedMaterials.reserve(placed.size());
            for (const RenderSubmesh* renderSubmesh : placed)
            {
                placedMaterials.push_back(RayMaterialSource{
                    renderSubmesh->drawSlot,
                    renderSubmesh->material,
                    renderSubmesh->alphaMode,
                    renderSubmesh->doubleSided,
                    renderSubmesh->rayBaseColor,
                    renderSubmesh->rayEmissive});
            }
            // Where every draw is now, for the worker's top level.
            std::vector<glm::mat4> rayModels;
            rayModels.reserve(newRenderSubmeshes.size());
            for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : newRenderSubmeshes)
            {
                rayModels.push_back(State().rendererWorld.GetSubmeshModelMatrix(renderSubmesh->entity, renderSubmesh->motionKey.submeshOrdinal));
            }
            m_rayScene->SetContent(
                std::move(raySubmeshes), std::move(rayModels), m_uniformBuffer->GetDrawCapacity(), placedMaterials, releasedSlots);
        }
        catch (...)
        {
            for (const RenderSubmesh* renderSubmesh : placed)
            {
                ReleaseDrawSlot(renderSubmesh->drawSlot);
                renderSubmesh->drawSlot = RenderSubmesh::kNoDrawSlot;
            }
            throw;
        }

        // References: the new draws take theirs on their textures and material sets, the dropped ones
        // give theirs back with their slots.
        for (const RenderSubmesh* renderSubmesh : placed)
        {
            for (const std::string& key : renderSubmesh->textureKeys)
            {
                ++m_textureStore.at(key).references;
            }
            m_materialSets->Retain(renderSubmesh->materialSet);
        }
        for (const RenderSubmesh* renderSubmesh : dropped)
        {
            ReleaseDrawSlot(renderSubmesh->drawSlot);
            renderSubmesh->drawSlot = RenderSubmesh::kNoDrawSlot;
            for (const std::string& key : renderSubmesh->textureKeys)
            {
                if (auto entry = m_textureStore.find(key); entry != m_textureStore.end() && entry->second.references > 0)
                {
                    --entry->second.references;
                }
            }
            m_materialSets->Release(renderSubmesh->materialSet);
            m_liveSubmeshes.erase(renderSubmesh->revision);
        }
        for (const RenderSubmesh* renderSubmesh : placed)
        {
            m_liveSubmeshes.emplace(renderSubmesh->revision, renderSubmesh->shared_from_this());
        }
        // The material sets no draw names any more go, before the textures they name are destroyed.
        m_materialSets->FreeUnreferenced();
    }

    // The textures no draw names any more, now that the material sets that named them are gone and the
    // frames that sampled them have finished.
    DropUnreferencedTextures();
    const bool newWorld = keptSubmeshCount * 2 < newRenderSubmeshes.size();
    m_renderSubmeshes = std::move(newRenderSubmeshes);
    // New content may be a different world (a scene that finished loading in the background while
    // the startup scene was on screen). The long-term exposure restarts its warm-up, so it catches
    // up at the short-term rates instead of keeping the old world's light for minutes; the view
    // itself does not snap. A streamed world's cells coming and going is the same world.
    if (newWorld)
    {
        m_autoExposureState.meteredSeconds = 0.0f;
    }
}

std::vector<VulkanDrawItem> VulkanRenderer::BuildDrawItems(
    uint32_t imageIndex,
    std::span<const glm::mat4> models,
    const glm::mat4& viewProjection) const
{
    std::vector<VulkanDrawItem> unsorted;
    std::vector<MaterialDrawSortKey> sortKeys;
    unsorted.reserve(m_renderSubmeshes.size());
    sortKeys.reserve(m_renderSubmeshes.size());
    const ViewFrustum frustum(viewProjection);

    for (size_t submeshIndex = 0; submeshIndex < m_renderSubmeshes.size(); ++submeshIndex)
    {
        const RenderSubmesh& renderSubmesh = *m_renderSubmeshes[submeshIndex];
        const glm::mat4& model = models[submeshIndex];
        const glm::vec3 worldCenter = glm::vec3(model * glm::vec4(renderSubmesh.localBoundsCenter, 1.0f));
        const float worldRadius =
            renderSubmesh.localBoundsRadius *
            std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])), glm::length(glm::vec3(model[2]))});
        if (!frustum.IntersectsSphere(worldCenter, worldRadius))
        {
            continue;
        }
        ObjectPushConstants drawConstants{};
        drawConstants.model = model;
        const MaterialPipelineKey pipelineKey{
            renderSubmesh.alphaMode,
            renderSubmesh.doubleSided};
        const glm::vec4 viewCenter =
            State().viewportMatrices.view *
            drawConstants.model *
            glm::vec4(renderSubmesh.localBoundsCenter, 1.0f);
        const bool forwardShaded = renderSubmesh.alphaMode != MaterialAlphaMode::Blend &&
                                   (renderSubmesh.material.shadingModel[0] & kShadingFlagForward) != 0u;
        const bool transmissive = forwardShaded && (renderSubmesh.material.shadingModel[0] & kShadingFlagTransmission) != 0u;
        sortKeys.push_back({pipelineKey, -viewCenter.z, forwardShaded, transmissive});
        unsorted.push_back(VulkanDrawItem{
            renderSubmesh.buffer->GetVertexHandle(),
            renderSubmesh.buffer->GetIndexHandle(),
            renderSubmesh.buffer->GetIndexCount(),
            renderSubmesh.materialSet,
            drawConstants,
            pipelineKey,
            // The draw slot, where this submesh's material, texture transforms and previous model
            // matrix are.
            renderSubmesh.drawSlot,
            forwardShaded,
            transmissive,
            MaterialScatters(renderSubmesh.material),
            renderSubmesh.decal});
    }

    std::vector<VulkanDrawItem> ordered;
    ordered.reserve(unsorted.size());
    for (size_t index : BuildMaterialDrawOrder(sortKeys))
    {
        ordered.push_back(std::move(unsorted[index]));
    }
    return ordered;
}

std::vector<ShadowDrawItem> VulkanRenderer::BuildShadowDrawItems(uint32_t imageIndex, std::span<const glm::mat4> models) const
{
    std::vector<ShadowDrawItem> items;
    items.reserve(m_renderSubmeshes.size());
    for (size_t submeshIndex = 0; submeshIndex < m_renderSubmeshes.size(); ++submeshIndex)
    {
        const RenderSubmesh& renderSubmesh = *m_renderSubmeshes[submeshIndex];
        // Blend materials are glass, foliage cards and the like; a solid shadow from them would be
        // wrong more often than none, so they cast none.
        // Transmissive surfaces let most light through; they cast none either.
        if (renderSubmesh.alphaMode == MaterialAlphaMode::Blend ||
            (renderSubmesh.material.shadingModel[0] & kShadingFlagTransmission) != 0u)
        {
            continue;
        }

        ShadowDrawItem item{};
        item.vertexBuffer = renderSubmesh.buffer->GetVertexHandle();
        item.positionBuffer = renderSubmesh.buffer->GetPositionHandle();
        item.indexBuffer = renderSubmesh.buffer->GetIndexHandle();
        item.indexCount = renderSubmesh.buffer->GetIndexCount();
        item.model = models[submeshIndex];
        item.worldBoundsCenter = glm::vec3(item.model * glm::vec4(renderSubmesh.localBoundsCenter, 1.0f));
        // The largest axis scale keeps the sphere enclosing under non-uniform scale.
        item.worldBoundsRadius =
            renderSubmesh.localBoundsRadius *
            std::max({glm::length(glm::vec3(item.model[0])),
                      glm::length(glm::vec3(item.model[1])),
                      glm::length(glm::vec3(item.model[2]))});
        item.alphaMask = renderSubmesh.alphaMode == MaterialAlphaMode::Mask;
        item.materialDescriptorSet = renderSubmesh.materialSet;
        std::memcpy(item.material.baseColorFactor, renderSubmesh.material.baseColorFactor, sizeof(item.material.baseColorFactor));
        std::memcpy(item.material.nodeGraphFactors, renderSubmesh.material.nodeGraphFactors, sizeof(item.material.nodeGraphFactors));
        item.material.alphaCutoff = renderSubmesh.material.alphaCutoff;
        // The alpha test samples the base colour where the main passes do.
        std::memcpy(item.baseColorTransform, &renderSubmesh.textureTransforms.rows[0], sizeof(item.baseColorTransform));
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
        m_gpuTimer->Mark(commandBuffer, ScenePassName(id));
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

void VulkanRenderer::ReportDroppedClusterLights(uint32_t droppedCount)
{
    // Logged when the count changes, like ReportDroppedLights.
    if (droppedCount == m_droppedClusterLightCount)
    {
        return;
    }
    m_droppedClusterLightCount = droppedCount;
    if (droppedCount > 0)
    {
        LOG_WARN(
            "The light cluster index list is full: {} light-cluster pairs are left out, so some lights stop short of their range",
            droppedCount);
    }
    else
    {
        LOG_INFO("Every light fits in the light cluster index list again");
    }
}

void VulkanRenderer::ReportDroppedLocalShadows(uint32_t droppedCount)
{
    // Logged when the count changes, like ReportDroppedLights.
    if (droppedCount == m_droppedLocalShadowCount)
    {
        return;
    }
    m_droppedLocalShadowCount = droppedCount;
    if (droppedCount > 0)
    {
        LOG_WARN("The local shadow atlas is full: {} lights in view cast no shadow", droppedCount);
    }
    else
    {
        LOG_INFO("Every shadow casting local light in view fits in the shadow atlas again");
    }
}

glm::mat3 VulkanRenderer::UpdateWhiteBalance()
{
    Camera& camera = State().camera;
    const AutoWhiteBalanceSettings& settings = camera.autoWhiteBalance;
    if (!settings.enabled || State().renderDebug.khronosReference)
    {
        // Off shows the illuminant as it is; turned back on, the white point adapts from where it
        // was rather than from D65.
        camera.adaptedWhiteKelvin = CorrelatedColorTemperature(kD65WhiteXy);
        return glm::mat3(1.0f);
    }

    // The view's colour from the histogram this slot recorded kMaxFramesInFlight frames ago (its
    // fence has signaled); the light references are the previous frame's. At these rates that is
    // moot.
    m_whiteBalanceReferences.frameColorRgb.reset();
    if (m_exposurePass != nullptr)
    {
        m_whiteBalanceReferences.frameColorRgb = m_exposurePass->GetFrameColor(m_commandContext->GetCurrentFrame());
    }
    const glm::vec2 target = EstimateIlluminantXy(m_whiteBalanceReferences);
    m_adaptedWhiteXy = m_adaptedWhiteXy.has_value()
                           ? AdaptWhitePointXy(*m_adaptedWhiteXy, target, State().frameDeltaSeconds, settings.adaptPerSecond)
                           : target;
    camera.adaptedWhiteKelvin = CorrelatedColorTemperature(*m_adaptedWhiteXy);
    return WhiteBalanceMatrix(*m_adaptedWhiteXy, settings.degree, DaylightXy(settings.targetKelvin));
}

void VulkanRenderer::UpdateAutoExposure(uint32_t frameSlot)
{
    Camera& camera = State().camera;
    const AutoExposureSettings& settings = camera.autoExposure;
    if (State().renderDebug.khronosReference)
    {
        // The Sample Viewer's exposure 1.0: an HDRI texel of 1 exposed to 1. Without an HDRI the
        // exposure is left where it is.
        const SceneEnvironment environment = State().editorWorld ? EditorWorld().GetEnvironment() : SceneEnvironment{};
        if (environment.mode == EnvironmentMode::Hdri)
        {
            camera.exposureEv100 = KhronosReferenceEv100(environment.hdri.intensity);
        }
        return;
    }
    if (!settings.enabled || !m_exposurePass)
    {
        // Manual mode: exposureEv100 is the user's. When auto exposure is turned back on it
        // adapts from that value rather than snapping.
        return;
    }

    // The slot's fence has signaled, so its histogram is the one it recorded kMaxFramesInFlight
    // frames ago. Empty means nothing but background was visible; the exposure then holds.
    const std::span<const uint32_t> histogram = m_exposurePass->GetHistogram(frameSlot);
    const std::optional<float> target = MeterTargetEv100(histogram, settings);
    if (!target.has_value())
    {
        return;
    }

    // The long-term stage meters the frame together with the sun and the sky; the view then moves
    // at most shortTermRangeEv away from it (see StepAutoExposure).
    ExposureReferences references = m_exposureReferences;
    references.frameLog2Luminance =
        MeterAverageLog2Luminance(histogram, settings.lowPercentile, settings.highPercentile);
    camera.exposureEv100 = StepAutoExposure(
        m_autoExposureState,
        camera.exposureEv100,
        *target,
        MeterLongTermTargetEv100(references, settings),
        State().frameDeltaSeconds,
        settings);
    camera.adaptedLongTermEv100 = m_autoExposureState.longTermEv100;
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
