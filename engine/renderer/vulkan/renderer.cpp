#include "renderer.h"

#include "imgui_nvrhi.h"

#include <stb_image_write.h>
#include "memory_pool.h"
#include "viewport_capture.h"

#include <engine/renderer/view_frustum.h>
#include <engine/renderer/environment_brdf.h>
#include <engine/renderer/ltc_table.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/renderer/render_features.h>
#include <engine/renderer/scene_lighting.h>
#include <engine/renderer/tyre_deformation.h>

#include <engine/logic/editor_world.h>
#include <engine/scene/scene_components.h>
#include <engine/scene/sun_position.h>
#include <imgui.h>
#include <engine/asset/compressed_texture_cache.h>
#include <engine/asset/texture_preparation.h>
#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/core/threading/task_system.h>
#include <engine/platform/window/window.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
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

// The sRGB format with the same bytes as an 8-bit UNORM one.
VkFormat SrgbFormatOf(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_SRGB;
    default:
        return format;
    }
}

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
    // Combined as sRGB-encoded values (detail_layers.slang), so read undecoded.
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

// The Graphics Debug window's line on GPU memory and the world's streaming radius.
std::string FormatGpuMemoryStatus(const GpuMemoryReport& report, const WorldStreamingState& streaming)
{
    if (report.serial == 0)
    {
        return {};
    }
    std::string status = std::format(
        "GPU memory: {} / {} MB (world {} MB, fullscreen reserve {} MB)",
        report.usage >> 20,
        report.budget >> 20,
        report.worldBytes >> 20,
        report.reserve >> 20);
    if (!streaming.cells.empty())
    {
        status += std::format("\nStreaming radius {:.0f} m, {} cells in high detail", streaming.budgetRadius, streaming.HighDetailCount());
    }
    return status;
}

// Logs a frame long enough to have stalled the editor. Texture work belongs on the preparation
// queue; this is where a regression back onto the frame loop shows up.
// The frame's loops over every render submesh run in ranges of this many on the task system.
constexpr uint32_t kSubmeshesPerTask = 512;
// Empty memory pool blocks kept for the next allocations (64 MiB each) before they go back to the
// driver, one a frame.
constexpr size_t kSpareMemoryBlocks = 4;

void MixHashWord(uint64_t& hash, uint64_t word)
{
    hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    hash *= 0xff51afd7ed558ccdull;
}

// HashShadowCasters' hash of one run of casters.
uint64_t HashShadowCasterRange(std::span<const ShadowDrawItem> items)
{
    // Eight bytes a step: a byte at a time took milliseconds over a map's tens of thousands of casters.
    uint64_t hash = 14695981039346656037ull;
    const auto mixWord = [&hash](uint64_t word)
    {
        MixHashWord(hash, word);
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

// A key that changes whenever the shadow casters do: which meshes, where, and how they alpha test.
// FNV-1a over the fields that reach the shadow map. Not the material descriptor set, which is one
// per swapchain image and would change the key every frame. Hashed in chunks of a fixed size on the
// task system, then the chunks in order, so the key does not depend on how the work was split.
uint64_t HashShadowCasters(std::span<const ShadowDrawItem> items)
{
    constexpr size_t kCastersPerChunk = 1024;
    const uint32_t chunkCount = static_cast<uint32_t>((items.size() + kCastersPerChunk - 1) / kCastersPerChunk);
    std::vector<uint64_t> chunkHashes(chunkCount);
    TaskSystem::ParallelFor(chunkCount, 1, [&](uint32_t begin, uint32_t end)
                            {
                                for (uint32_t chunk = begin; chunk < end; ++chunk)
                                {
                                    const size_t first = chunk * kCastersPerChunk;
                                    chunkHashes[chunk] = HashShadowCasterRange(items.subspan(first, std::min(kCastersPerChunk, items.size() - first)));
                                }
                            });
    uint64_t hash = 14695981039346656037ull;
    for (const uint64_t chunkHash : chunkHashes)
    {
        MixHashWord(hash, chunkHash);
    }
    MixHashWord(hash, items.size());
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
            const VulkanTexture* texture = textures[textureIndex];
            return BindTexture(texture->GetImageView(), texture->GetNvrhiTexture(), samplerCache.Get(slots.samplers[slot]));
        };
        // The detail maps come outside glTF's texture slots: always the default sampler (repeat,
        // linear, mipmapped), which their tiling needs.
        const auto bindDefault = [&](uint32_t textureIndex)
        {
            const VulkanTexture* texture = textures[textureIndex];
            return BindTexture(texture->GetImageView(), texture->GetNvrhiTexture(), samplerCache.Get(TextureSampler{}));
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

// Each material binding's sampler as an index into set 0's table, a byte each, four to a word
// (scene_common.slang's MaterialSamplerIndex): the glTF slots' own, the detail maps the default, as
// BuildMaterialTextureBindings pairs them.
void PackMaterialSamplerIndices(const MaterialTextureSlots& slots, uint32_t (&packed)[8])
{
    static_assert(kMaterialTextureBindingCount == 8 * 4, "Four sampler indices a word");
    static_assert(VulkanSamplerCache::kSamplerCount <= 256, "A sampler index must fit a byte");
    std::fill(std::begin(packed), std::end(packed), 0u);
    for (uint32_t binding = 0; binding < kMaterialTextureBindingCount; ++binding)
    {
        const TextureSampler sampler = binding < slots.samplers.size() ? slots.samplers[binding] : TextureSampler{};
        packed[binding / 4] |= VulkanSamplerCache::IndexOf(sampler) << ((binding % 4) * 8);
    }
}

// The state a pass writes a target in, by its kind (render_target_layout.h's write layouts).
nvrhi::ResourceStates GetWriteState(RenderTargetId target)
{
    switch (GetRenderTargetKind(target))
    {
    case RenderTargetKind::Depth:
        return nvrhi::ResourceStates::DepthWrite;
    case RenderTargetKind::Storage:
        return nvrhi::ResourceStates::UnorderedAccess;
    default:
        return nvrhi::ResourceStates::RenderTarget;
    }
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

}

VulkanRenderer::VulkanRenderer(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    std::optional<std::string> startupModelPath,
    RenderBackendType backendType)
    : EditorRenderBackendBase(window, std::move(sharedState), backendType, std::move(startupModelPath))
{
    // The graphics API's device (docs/design/2026-10-09-d3d12-backend-design.md); everything after it
    // is the same on both, but for the Vulkan-only pieces (NGX's Vulkan entry points, the Vulkan
    // acceleration structures), which ask the device for Vulkan's objects.
#if MINIENGINE_WITH_D3D12
    m_nvrhi = backendType == RenderBackendType::D3D12 ? CreateD3D12GpuDevice(State().rayQuery)
                                                      : CreateVulkanGpuDevice(GetWindow().GetSDLWindow(), State().rayQuery);
#else
    m_nvrhi = CreateVulkanGpuDevice(GetWindow().GetSDLWindow(), State().rayQuery);
#endif
    LOG_INFO("Rendering with {} on {}", m_nvrhi->IsVulkan() ? "Vulkan" : "Direct3D 12", m_nvrhi->GetAdapterName());
    if (VulkanDevice* vulkanDevice = m_nvrhi->GetVulkanDevice(); vulkanDevice != nullptr)
    {
        VulkanInstance* vulkanInstance = m_nvrhi->GetVulkanInstance();
        m_dlss = std::make_unique<VulkanDlss>(
            vulkanInstance->GetHandle(),
            vulkanDevice->GetPhysicalDevice(),
            vulkanDevice->GetHandle(),
            vulkanDevice->GetQueueFamilies().graphicsFamily.value(),
            vulkanDevice->GetGraphicsQueue(),
            vulkanInstance->OptionalExtensionsEnabled() && vulkanDevice->OptionalExtensionsEnabled());
    }
    else
    {
        m_dlss = std::make_unique<VulkanDlss>(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, false);
    }
    m_imguiLayer = std::make_unique<VulkanImGuiLayer>(
        GetWindow().GetSDLWindow(), m_nvrhi->Get(), static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    m_samplerCache = std::make_unique<VulkanSamplerCache>(m_nvrhi->Get(), m_nvrhi->GetMaxSamplerAnisotropy());
    CreateDeviceResources();
    // Half the hardware threads: the rest stay free for the frame loop and for the band-parallel
    // encoding inside each texture.
    const bool compressTextures = m_nvrhi->SupportsBlockCompression();
    m_texturePreparation = std::make_unique<TexturePreparationQueue>(
        [compressTextures, cacheDirectory = DefaultTextureCacheDirectory()](const std::string& path, TextureUsage usage)
        {
            return PrepareTexture(path, usage, compressTextures, cacheDirectory);
        },
        std::max(1u, std::thread::hardware_concurrency() / 2));
    CreateSwapchainResources();
    // The startup scene uploads synchronously: there is nothing on screen to keep responsive yet,
    // and it has no texture files.
    RenderFramePacket& startup = m_framePackets[0];
    startup.renderSubmeshes = RenderWorld().SnapshotRenderSubmeshes();
    startup.transforms.Capture(RenderWorld(), *startup.renderSubmeshes);
    UploadSceneResources(startup);
    m_renderThread = std::make_unique<RenderThread>(State().renderThread ? RenderThread::Mode::Threaded : RenderThread::Mode::Inline);
    LOG_INFO("Rendering on {}", State().renderThread ? "a render thread" : "the main thread (--no-render-thread)");
}

VulkanRenderer::~VulkanRenderer()
{
    // The render thread finishes the frame in hand and stops; a failure it had is dropped, as the
    // device goes anyway.
    if (m_renderThread)
    {
        try
        {
            m_renderThread->WaitIdle();
        }
        catch (const std::exception& error)
        {
            LOG_ERROR("The render thread failed: {}", error.what());
        }
        m_renderThread.reset();
    }
    if (m_frameTimesFile != nullptr)
    {
        std::fclose(m_frameTimesFile);
        m_frameTimesFile = nullptr;
    }
    // Its last frames are read back from the device about to be torn down.
    try
    {
        StopVideoRecording();
        StopQuadRecording();
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("Failed to finish the recording: {}", error.what());
    }
    // Joins the workers before anything they might still be preparing for is torn down.
    m_texturePreparation.reset();

    if (m_nvrhi)
    {
        m_nvrhi->Get()->waitForIdle();
    }
    // The device is idle: what was retired goes now, before the caches and the ray scene it names.
    m_uploadBatches.clear();
    m_preparedBuffers.clear();
    m_meshesToUpload.clear();
    m_retireQueue.Flush();
    m_videoReadback.reset();
    m_quadReadback.reset();

    DestroyDescriptorResources();
    m_forwardPipelines.reset();
    m_geometryPipelines.reset();
    m_decalPipelines.reset();
    // Its ImGui binding goes while the ImGui Vulkan backend is still up: DestroySwapchainResources
    // shuts that backend down.
    ReleaseMinimapTexture();
    DestroySwapchainResources();
    m_view.ClearPasses();
    m_view.targets.reset();
    m_imguiLayer.reset();
    m_textureStore.clear();
    m_stagedTextures.clear();
    m_renderSubmeshes.clear();
    m_liveSubmeshes.clear();
    m_materialSets.reset();
    m_samplerCache.reset();
    DestroyDeviceResources();
    m_dlss.reset();
    m_uploadPool.reset();
    m_nvrhi.reset();
}

void VulkanRenderer::DrawFrame()
{
    const FrameStallReporter stallReporter;
    const auto frameStart = std::chrono::steady_clock::now();
    m_mainStages.BeginFrame();

    if (!TickSharedFrame())
    {
        return;
    }
    m_mainStages.Mark("Tick");
    ApplyRenderFeedback();

    bool contentChanged = ProcessPendingOperations();
    m_mainStages.Mark("PendingOperations");
    EditorWorld().FlushDirtyTransforms();
    m_mainStages.Mark("SceneUpdates");

    // A swapchain that no longer matches the window is rebuilt before drawing rather than after a
    // present reports it: drawing into the old size first leaves the newly exposed area unpainted
    // for a frame, which shows on every step of a live resize.
    // Switching HDR output changes the swapchain's format, and with it everything built on it.
    // The rebuild replaces the ImGui backend and its font texture, so it runs here: with no frame on
    // its way, and before this frame's UI names the font.
    // A minimized window's surface is 0 x 0 while SDL can still report its last size (Windows):
    // nothing can be made or drawn at that size, so frames wait for the window to come back.
    const VkExtent2D wantedExtent = WantedSwapchainExtent();
    if (wantedExtent.width == 0 || wantedExtent.height == 0)
    {
        return;
    }
    const VkExtent2D currentExtent = m_swapchain->GetExtent();
    if (m_swapchainOutOfDate.exchange(false) || wantedExtent.width != currentExtent.width ||
        wantedExtent.height != currentExtent.height || State().renderDebug.hdrOutput != m_swapchainHdrRequested)
    {
        m_renderThread->RunExclusive([this]()
                                     {
                                         RecreateSwapchain();
                                     });
        m_mainStages.Mark("RecreateSwapchain");
    }

    // The size the render thread resizes the scene targets to before drawing this frame.
    const RenderExtent viewportExtent = State().fixedViewportExtent.value_or(State().requestedViewportExtent);
    UpdateViewportMatrices(viewportExtent);

    m_imguiLayer->BeginFrame();
    State().editorUi.BeginFrame(GetWindow().GetSDLWindow(), State().engineSettings);
    // The render thread owns the viewport's and the minimap's textures; the UI names them by ID.
    State().editorUi.SetMinimapTexture(m_minimapAvailable ? kMinimapTextureId : ImTextureID{});
    State().editorUi.SetSelectionOutlineTexture(kSelectionOutlineTextureId);
    // Fixed once NGX has started, before the render thread exists.
    State().editorUi.SetDlssStatus(m_dlss->IsAvailable(), m_dlss->IsRayReconstructionAvailable(), m_dlss->Status());
    State().editorUi.SetPathTracingStatus(m_rayScene->HasHardwareRayTracing(), m_pathTracingStatusShown, m_pathTracingProgressShown);
    State().editorUi.SetGpuMemoryStatus(FormatGpuMemoryStatus(State().gpuMemory, State().worldStreaming));
    State().editorUi.SetGpuMemory(State().gpuMemory);
    const EditorUiFrameResult uiFrame = DrawEditorUi(kViewportTextureId, viewportExtent);
    ApplyUiActions(uiFrame);
    EditorWorld().FlushDirtyTransforms();
    // A change the UI made goes with this frame: one that needs no new texture file commits in it.
    contentChanged |= State().renderablesDirty;
    State().renderablesDirty = false;
    ImGui::Render();
    ApplyImGuiTextureRequests(*ImGui::GetDrawData());
    m_mainStages.Mark("EditorUi");

    // The packet two frames back: Submit waited for the render thread to finish it.
    RenderFramePacket& packet = m_framePackets[(m_frameSerial + 1) % m_framePackets.size()];
    BuildFramePacket(packet, contentChanged, viewportExtent);
    if (contentChanged)
    {
        // Loading until the render thread reports on this frame (ApplyRenderFeedback).
        m_lastContentSerial = packet.serial;
        State().rayScenePending = true;
    }
    m_mainStages.Mark("BuildFrame");
    m_renderThread->Submit([this, &packet]()
                           {
                               RenderFrame(packet);
                           });
    // Waiting for the render thread to finish the previous frame (in inline mode: drawing this one).
    m_mainStages.Mark("WaitForRender");

    const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
    if (m_mainFrameMs.size() < VulkanGpuTimer::kAverageFrames)
    {
        m_mainFrameMs.push_back(frameMs);
    }
    else
    {
        m_mainFrameMs[m_mainFrameCursor % VulkanGpuTimer::kAverageFrames] = frameMs;
    }
    ++m_mainFrameCursor;
}

void VulkanRenderer::ApplyImGuiTextureRequests(const ImDrawData& drawData)
{
    // ImGui asks the backend through the draw data to make, update or destroy its textures (the font
    // atlas grows as text needs new glyphs). The backend does that with the device and the queue, on
    // textures the ImGui context owns, so it happens here on the main thread with the render thread
    // idle, on the frames that ask; the frame's copy of the draw data carries no requests.
    if (drawData.Textures == nullptr ||
        std::none_of(drawData.Textures->begin(), drawData.Textures->end(), [](const ImTextureData* texture)
                     {
                         return texture->Status != ImTextureStatus_OK;
                     }))
    {
        return;
    }
    m_renderThread->RunExclusive([this, &drawData]()
                                 {
                                     for (ImTextureData* texture : *drawData.Textures)
                                     {
                                         if (texture->Status != ImTextureStatus_OK)
                                         {
                                             m_imguiLayer->GetRenderer().UpdateTexture(texture);
                                         }
                                     }
                                 });
}

void VulkanRenderer::ApplyRenderFeedback()
{
    RenderFeedback feedback;
    {
        const std::lock_guard lock(m_feedbackMutex);
        feedback = m_feedback;
        m_feedback.outOfMemory.reset();
    }
    // A change of content the render thread has not drawn yet is still loading.
    State().rayScenePending = feedback.rayScenePending || feedback.serial < m_lastContentSerial;
    if (feedback.serial == 0)
    {
        return;
    }
    Camera& camera = State().camera;
    // Auto exposure and the Khronos reference view set the EV. In manual mode it is the user's, which
    // the render thread took from the frame.
    if (camera.autoExposure.enabled || State().renderDebug.khronosReference)
    {
        camera.exposureEv100 = feedback.exposureEv100;
    }
    camera.adaptedLongTermEv100 = feedback.adaptedLongTermEv100;
    camera.adaptedWhiteKelvin = feedback.adaptedWhiteKelvin;
    State().sceneUploadStatus = feedback.sceneUploadStatus;
    if (feedback.outOfMemory.has_value())
    {
        if (*feedback.outOfMemory)
        {
            State().lastModelLoadError = kOutOfMemoryReport;
        }
        // The screen matches the scene again, so a report of it not matching is now stale.
        else if (State().lastModelLoadError == kOutOfMemoryReport)
        {
            State().lastModelLoadError.clear();
        }
    }
    m_minimapAvailable = feedback.minimapLoaded;
    State().gpuMemory = feedback.gpuMemory;
    m_pathTracingStatusShown = feedback.pathTracingStatus;
    m_pathTracingProgressShown = feedback.pathTracingProgress;
    ReportPhotoView(feedback.photoView);
    if (feedback.outOfMemory.value_or(false))
    {
        // World streaming gives memory back before it asks for more.
        State().worldStreaming.uploadOutOfMemory = true;
    }
}

void VulkanRenderer::RestartTemporalEffects()
{
    const auto restart = [](VulkanSceneView& view)
    {
        view.ResetHistories();
        view.taaFrameIndex = 0;
        view.aoFrameIndex = 0;
        if (view.atmosphere)
        {
            view.atmosphere->RestartHistory();
        }
    };
    restart(m_view);
    for (const std::unique_ptr<VulkanSceneView>& view : m_captureViews)
    {
        restart(*view);
    }
    m_view.pathTraceAccumulation.Reset();
    m_view.dlssResetPending = true;
}

void VulkanRenderer::BuildFramePacket(RenderFramePacket& packet, bool contentChanged, RenderExtent viewportExtent)
{
    packet.serial = ++m_frameSerial;
    packet.deltaSeconds = State().frameDeltaSeconds;
    // Again after the UI, which may have moved the camera (framing the selection, the mouse wheel).
    UpdateViewportMatrices(viewportExtent);
    packet.camera = State().camera;
    packet.viewportMatrices = State().viewportMatrices;
    packet.renderDebug = State().renderDebug;
    packet.temporalRestart = State().temporalRestart;
    packet.viewportExtent = viewportExtent;
    packet.displayExtent = {};
    if (State().fixedViewportExtent.has_value() || State().renderDebug.viewportResolution.fixed)
    {
        // A fixed resolution stays as it is fullscreen: nothing to reserve room for.
        packet.displayExtent = viewportExtent;
    }
    else if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(GetWindow().GetSDLWindow())); mode != nullptr)
    {
        const float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
        packet.displayExtent = {
            static_cast<uint32_t>(static_cast<float>(mode->w) * density),
            static_cast<uint32_t>(static_cast<float>(mode->h) * density)};
    }

    IEditorWorld* const world = State().editorWorld.get();
    packet.environment = world != nullptr ? world->GetEnvironment() : SceneEnvironment{};
    packet.minimapPath.clear();
    if (world != nullptr && world->GetMinimap().IsValid())
    {
        packet.minimapPath = world->GetMinimap().image;
    }
    packet.lights = world != nullptr ? CollectSceneLights(*world, State().rendererWorld) : CollectedSceneLights{};
    packet.selectedEntity = world != nullptr && world->HasSelection() ? world->GetSelectedEntity() : entt::null;
    packet.uiScale = State().editorUi.GetEffectiveUiScale();
    packet.viewportOutputScale = State().viewportOutputScale;

    packet.contentChanged = contentChanged;
    packet.renderSubmeshes = State().rendererWorld.SnapshotRenderSubmeshes();
    packet.transforms.Capture(State().rendererWorld, *packet.renderSubmeshes);
    packet.ui.Capture(*ImGui::GetDrawData());
    packet.captureViews = CaptureViews();
}

void VulkanRenderer::RenderFrame(RenderFramePacket& packet)
{
    const FrameStallReporter stallReporter;
    const auto frameStart = std::chrono::steady_clock::now();
    m_cpuStages.BeginFrame();

    // What the frames that have finished no longer need, draw slots among it, before this frame's
    // change of content asks for some.
    m_retireQueue.Collect(m_commandContext->CompletedSubmits());
    // The memory pool's blocks that emptied, back to the driver one a frame beyond a few spares (each
    // vkFreeMemory holds the driver for about a millisecond; compaction empties dozens at once).
    VulkanMemoryPool::ReleaseEmptyBlocks(m_nvrhi->Get(), kSpareMemoryBlocks, 1);

    if (packet.contentChanged)
    {
        RequestSceneUpload(packet);
        m_cpuStages.Mark("RequestUpload");
    }
    // Every frame: stages textures the workers finished, and commits a pending change once its
    // last texture is ready.
    PumpSceneUpload(packet);
    m_cpuStages.Mark("PumpUpload");

    // The main thread rebuilds the swapchain before its next frame; until then nothing is drawn.
    if (m_swapchainOutOfDate.load())
    {
        PublishFeedback(packet);
        return;
    }
    SyncSceneTargets(packet.viewportExtent, packet.renderDebug);
    SyncCaptureViews(packet.captureViews);
    if (packet.temporalRestart != m_temporalRestart)
    {
        m_temporalRestart = packet.temporalRestart;
        RestartTemporalEffects();
    }

    uint32_t imageIndex = 0;
    const auto waitStart = std::chrono::steady_clock::now();
    const SwapchainStatus acquireResult = m_commandContext->AcquireNextImage(imageIndex);
    const double waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    m_cpuStages.Mark("Acquire");
    if (acquireResult == SwapchainStatus::OutOfDate)
    {
        m_swapchainOutOfDate = true;
        PublishFeedback(packet);
        return;
    }
    // AcquireNextImage waited on this slot's fence: the video frames its last use copied are ready.
    SubmitVideoFrame(m_commandContext->GetCurrentFrame());
    SubmitQuadVideoFrame(m_commandContext->GetCurrentFrame());

    UpdateAutoExposure(m_view, packet.camera, packet, m_commandContext->GetCurrentFrame());
    // The slot's fence has signalled: the secondary command buffers its last frame used are free.
    if (m_parallelRecorder)
    {
        m_parallelRecorder->BeginFrame(m_commandContext->GetCurrentFrame());
    }
    UpdateMinimapTexture(packet.minimapPath);
    m_cpuStages.Mark("FrameStart");

    SharedFrameState shared;
    shared.imageIndex = imageIndex;
    shared.frameSlot = m_commandContext->GetCurrentFrame();
    const CollectedSceneLights& sceneLights = packet.lights;
    shared.lightSelection = SelectSceneLights(sceneLights.candidates, packet.camera.position, kMaxSceneLights);
    const SceneLightSelection& lightSelection = shared.lightSelection;
    ReportDroppedLights(lightSelection.droppedCount);
    std::vector<GpuLightData>& selectedLights = shared.selectedLights;
    selectedLights.reserve(lightSelection.selected.size());
    for (uint32_t index : lightSelection.selected)
    {
        selectedLights.push_back(sceneLights.gpuLights[index]);
        if (sceneLights.candidates[index].type == LightType::Directional)
        {
            ++shared.directionalLightCount;
        }
    }

    shared.shadowLightIndex = SelectShadowCasterLight(sceneLights.candidates, lightSelection);
    const int32_t shadowLightIndex = shared.shadowLightIndex;
    const SceneEnvironment& environment = packet.environment;
    // At night the moon stands in for the sun: everything below that takes the sun (the shadows, the
    // sky, the clouds, the fog, the exposure) takes the moon instead.
    if (shadowLightIndex >= 0)
    {
        if (const std::optional<SkyLight> moon = ComputeMoonlight(environment.timeOfDay); moon.has_value())
        {
            GpuLightData& light = selectedLights[static_cast<size_t>(shadowLightIndex)];
            light.directionAndType = glm::vec4(-moon->directionToLight, light.directionAndType.w);
            const float intensity = std::max({moon->illuminance.r, moon->illuminance.g, moon->illuminance.b});
            light.colorAndIntensity = intensity > 0.0f ? glm::vec4(moon->illuminance / intensity, intensity) : glm::vec4(0.0f);
        }
    }
    m_cpuStages.Mark("Lights");
    // Every draw's model matrix, once, for the shadow casters, the motion vectors, the ray scene and
    // the draw items.
    const uint32_t submeshCount = static_cast<uint32_t>(m_renderSubmeshes.size());
    shared.models.resize(submeshCount);
    shared.motionKeys.resize(submeshCount);
    shared.drawSlots.resize(submeshCount);
    TaskSystem::ParallelFor(submeshCount, kSubmeshesPerTask, [&](uint32_t begin, uint32_t end)
                            {
                                for (uint32_t index = begin; index < end; ++index)
                                {
                                    const RenderSubmesh& renderSubmesh = *m_renderSubmeshes[index];
                                    shared.models[index] = packet.transforms.GetSubmeshModelMatrix(renderSubmesh.entity, renderSubmesh.motionKey.submeshOrdinal);
                                    shared.motionKeys[index] = renderSubmesh.motionKey;
                                    shared.drawSlots[index] = renderSubmesh.drawSlot;
                                }
                            });
    const std::vector<glm::mat4>& models = shared.models;
    m_cpuStages.Mark("Models");
    // The casters, built here because which cascades each view's map keeps depends on them. The
    // shader samples the cascades as the map holds them, which for one Plan left waiting is its
    // previous matrix.
    shared.shadowDrawItems = BuildShadowDrawItems(imageIndex, models, packet.camera.position);
    m_cpuStages.Mark("ShadowDrawItems");
    shared.shadowCasterKey = HashShadowCasters(shared.shadowDrawItems);
    m_cpuStages.Mark("ShadowPlan");

    UpdateEnvironmentMap(environment);
    shared.environmentMode = EffectiveEnvironmentMode(environment);
    const EnvironmentMode environmentMode = shared.environmentMode;
    shared.atmosphereParameters = BuildAtmosphereParameters(environment.atmosphere);
    const AtmosphereParameters& atmosphereParameters = shared.atmosphereParameters;
    if (shadowLightIndex >= 0)
    {
        GpuLightData& sunLight = selectedLights[static_cast<size_t>(shadowLightIndex)];
        shared.sun = AtmosphereSun{
            -glm::normalize(glm::vec3(sunLight.directionAndType)),
            glm::vec3(sunLight.colorAndIntensity) * sunLight.colorAndIntensity.w};
        // A sun on the clock passes through the air in every sky mode, so it reddens low and sets
        // below the horizon with no atmosphere drawn.
        if (environmentMode == EnvironmentMode::Atmosphere || environment.timeOfDay.enabled)
        {
            // The light's intensity is the illuminance above the atmosphere; the scene receives
            // what gets through to the camera's altitude.
            const glm::vec3 camera = ToAtmosphereCameraPositionKm(atmosphereParameters, packet.camera.position);
            const float cosZenith = glm::dot(shared.sun->directionToSun, glm::normalize(camera));
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
    // The wind carries the clouds, their billows rise and their plumes live and die on the clouds'
    // own clock, once a frame whatever the views.
    m_cloudMotion = AdvanceCloudMotion(m_cloudMotion, environment, packet.deltaSeconds);
    if (environmentMode == EnvironmentMode::Hdri)
    {
        // The L0 band of the radiance SH is its average over the sphere times Y00 = 0.282095.
        constexpr glm::vec3 kLuma(0.2126f, 0.7152f, 0.0722f);
        const EnvironmentUniformData hdri = BuildViewEnvironment(shared, packet, packet.camera.position, 0);
        const glm::vec3 averageRadiance = glm::vec3(hdri.hdriIrradianceSh[0]) * 0.282095f;
        m_exposureReferences.skyLuminance = glm::dot(averageRadiance, kLuma);
        m_whiteBalanceReferences.skyIlluminanceRgb = averageRadiance * glm::pi<float>();
    }
    m_cpuStages.Mark("Environment");

    // The ray scene: a finished hierarchy build replaces the buffers every frame slot's set names,
    // then this frame's instances go into this slot's.
    if (m_rayScene->HasFinishedBuild())
    {
        m_rayScene->InstallBuild([this]()
                                 {
                                     m_commandContext->WaitForAllFrames();
                                     m_cpuStages.Mark("RayInstallWait");
                                 });
        m_cpuStages.Mark("RayInstall");
        // The probes keep the light they hold: most of it still holds (a streamed cell far away
        // changes nothing here), and each probe whose light the new content changes notices at its
        // next update and starts its average over (ddgi_update.comp). They judge whether they are
        // buried or empty afresh.
        ++m_ddgiGeometryEpoch;
        m_ddgiScheduler.GeometryChanged();
    }
    // Hardware bottom levels are built a few each frame; once the last of a content's is, the probes look
    // again, as they do when content installs.
    if (m_rayScene->TakeAccelerationCompleted())
    {
        ++m_ddgiGeometryEpoch;
        m_ddgiScheduler.GeometryChanged();
    }
    const std::span<const uint8_t> ddgiMoving = m_ddgiMovingInstances.Update(models);
    m_rayScene->UpdateInstances(m_commandContext->GetCurrentFrame(), models, ddgiMoving);
    m_cpuStages.Mark("RayInstances");

    // The lighting the probes hold, so they start their averages over when it changes: every
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
    m_ddgiLighting.Update(ddgiLighting);
    // A jump in how much light there is (the time of day scrubbed from afternoon to night, the sun
    // switched off) clears the probes: each restarts its own average on a new epoch, but its rays'
    // bounce reads its neighbours, which still hold the old light until their turn comes, and from
    // sunlight to moonlight (some 10^5 times darker) that kept a night scene lit for minutes.
    bool ddgiLightingJump = false;
    {
        float lightLevel = 0.0f;
        for (size_t index = 1; index + 2 < ddgiLighting.size(); index += 2)
        {
            const glm::vec4& colorAndIntensity = ddgiLighting[index];
            lightLevel += std::max({colorAndIntensity.r, colorAndIntensity.g, colorAndIntensity.b}) * colorAndIntensity.w;
        }
        const glm::vec4& ambient = ddgiLighting[ddgiLighting.size() - 2];
        const glm::vec4& hdri = ddgiLighting.back();
        lightLevel += std::max({ambient.r, ambient.g, ambient.b}) + std::max({hdri.r, hdri.g, hdri.b});
        if (m_ddgiLighting.Changed())
        {
            ddgiLightingJump = DdgiLightingJumped(m_ddgiLightLevel, lightLevel);
            m_ddgiLightLevel = lightLevel;
        }
        else if (m_ddgiLightLevel <= 0.0f)
        {
            m_ddgiLightLevel = lightLevel;
        }
    }
    const float ddgiHysteresis = std::clamp(packet.renderDebug.ddgi.hysteresis, 0.0f, 0.999f);
    const uint32_t ddgiLightingEpoch = m_ddgiLighting.Epoch();
    const uint32_t ddgiGeometryEpoch = m_ddgiGeometryEpoch;
    // What the probes this frame slot updated last time reported (its fence has signalled).
    m_ddgi->TakeFeedback(m_commandContext->GetCurrentFrame(), m_ddgiFeedbackSchedule, m_ddgiFeedback);

    // Which effects the settings leave a place for this frame, as the Graphics Debug panel shows them
    // (render_features.h): each runs where its switch and its feature are both on.
    const bool dlssEnabled = m_activeDlssMode != DlssMode::Off && m_dlss->HasFeature();
    RenderCapabilities& capabilities = shared.capabilities;
    capabilities.rayQueries = m_rayScene->HasHardwareRayTracing();
    capabilities.raySceneReady = m_rayScene->IsReady() && m_rayScene->GetTextureSet() != VK_NULL_HANDLE;
    capabilities.pathTracer = m_view.pathTracePass != nullptr && m_view.pathTracePass->IsSupported();
    capabilities.restirPt = m_view.restirPtPass != nullptr && m_view.restirPtPass->IsAvailable();
    capabilities.dlss = dlssEnabled;
    capabilities.dlssRayReconstruction = dlssEnabled && m_dlss->HasRayReconstruction();
    const RenderFeatures features = ResolveRenderFeatures(packet.renderDebug, capabilities);

    // DDGI: this frame's levels around the camera and the probes that update. Off in the Khronos
    // reference view, as the Sample Viewer has no GI, and until the ray scene can be traced.
    DdgiUniformData& ddgiData = shared.ddgiData;
    std::vector<uint32_t> ddgiSchedule;
    const DdgiSettings ddgiSettings = packet.renderDebug.ddgi;
    if (ddgiSettings.enabled && features.ddgi && m_rayScene->IsReady())
    {
        const uint32_t levelCount = static_cast<uint32_t>(std::clamp(ddgiSettings.levels, 1, static_cast<int>(kDdgiMaxLevels)));
        const float baseSpacing = std::clamp(ddgiSettings.baseSpacing, 0.25f, 8.0f);
        const glm::vec2 layout(static_cast<float>(levelCount), baseSpacing);
        if (layout != m_ddgiLayout || ddgiLightingJump)
        {
            m_ddgi->Invalidate();
            m_ddgiScheduler.Reset();
            m_ddgiLayout = layout;
        }
        std::array<DdgiLevel, kDdgiMaxLevels> levels{};
        for (uint32_t level = 0; level < levelCount; ++level)
        {
            levels[level] = ComputeDdgiLevel(packet.camera.position, std::ldexp(baseSpacing, static_cast<int>(level)));
            ddgiData.spacing[level] = levels[level].spacing;
            ddgiData.origins[level] = glm::vec4(glm::vec3(levels[level].origin), 0.0f);
        }
        const uint32_t budget = static_cast<uint32_t>(std::clamp(ddgiSettings.probesPerFrame, 64, static_cast<int>(VulkanDdgi::kMaxProbesPerFrame)));
        // Levels whose probes have converged refresh less (DdgiProbeScheduler); lighting that
        // changed, or instances that move, start them converging over.
        m_ddgiScheduler.ApplyFeedback(m_ddgiFeedbackSchedule, m_ddgiFeedback);
        if (m_ddgiLighting.Changed() ||
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

    // Local light shadows: the atlas tiles go to the selected local lights in the selection's order,
    // and each light learns its first tile through areaRightAxis.w (1 + tile, 0 for none). Planned
    // for the viewport's camera; every view samples the same atlas.
    const glm::mat4 viewProjection = packet.viewportMatrices.renderProjection * packet.viewportMatrices.view;
    if (packet.renderDebug.localLightShadows && features.localLightShadows)
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
        shared.gpuShadowTiles.reserve(plan.tiles.size());
        for (const LocalShadowTile& tile : plan.tiles)
        {
            shared.gpuShadowTiles.push_back(GpuLocalShadowTile{tile.viewProjection, tile.atlasRect, glm::vec4(tile.texelScale, tile.cubeFace ? 1.0f : 0.0f, 0.0f, 0.0f)});
        }
        shared.localShadowTiles = std::move(plan.tiles);
        ReportDroppedLocalShadows(plan.droppedCount);
    }
    else
    {
        ReportDroppedLocalShadows(0);
    }

    // Every skinned submesh posed by its entity's palette this frame, drawn or culled: it may still cast
    // a shadow into view. Without a palette (not yet ticked) it keeps the pose it has.
    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
    {
        if (!renderSubmesh->skinned || !renderSubmesh->skinningSet)
        {
            continue;
        }
        const std::vector<glm::mat4>* palette = packet.transforms.GetJointPalette(renderSubmesh->entity);
        if (palette == nullptr)
        {
            continue;
        }
        shared.skinningDispatches.push_back(VulkanSkinningPass::Dispatch{
            renderSubmesh->buffer.get(), renderSubmesh->skinningSet.Get(), palette, renderSubmesh->paletteOffset, renderSubmesh->jointCount});
    }
    // Every tyre too, deformed or not: one the car no longer squashes goes back to its shape at rest,
    // and its last frame's shape keeps rolling into its motion vectors.
    static const std::vector<glm::mat4> kTyreAtRest = []
    {
        const std::array<glm::mat4, 2> packed = PackTyreDeformation(TyreDeformation{});
        return std::vector<glm::mat4>(packed.begin(), packed.end());
    }();
    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
    {
        if (!renderSubmesh->tyre || !renderSubmesh->skinningSet)
        {
            continue;
        }
        const uint32_t first = renderSubmesh->motionKey.submeshOrdinal * 2;
        const std::vector<glm::mat4>* deformations = packet.transforms.GetTyreDeformations(renderSubmesh->entity);
        const bool deformed = deformations != nullptr && first + 2 <= deformations->size();
        shared.skinningDispatches.push_back(VulkanSkinningPass::Dispatch{
            renderSubmesh->buffer.get(), renderSubmesh->skinningSet.Get(), deformed ? deformations : &kTyreAtRest, deformed ? first : 0u, 2, true});
    }
    // From the viewport's histogram; every view is balanced the same.
    shared.whiteBalance = UpdateWhiteBalance(packet);

    // The capture views first, each from its own camera with its own exposure, then the viewport.
    std::vector<std::unique_ptr<PreparedView>> capturePrepared;
    for (size_t index = 0; index < m_captureViews.size(); ++index)
    {
        VulkanSceneView& view = *m_captureViews[index];
        const SceneCaptureView& capture = packet.captureViews[index];
        if (capture.resetHistory)
        {
            view.ResetHistories();
        }
        Camera camera = capture.camera;
        UpdateAutoExposure(view, camera, packet, shared.frameSlot);
        capturePrepared.push_back(PrepareView(view, camera, capture.matrices, false, shared, packet, capture.wholeExtent, &capture));
        if (capture.photo)
        {
            m_photoTile = capture.photoTile;
        }
    }
    m_cpuStages.Mark("CaptureViews");
    const std::unique_ptr<PreparedView> mainPrepared = PrepareView(m_view, packet.camera, packet.viewportMatrices, true, shared, packet);
    const ScenePassFrameContext& frame = mainPrepared->frame;
    const EnvironmentUniformData& environmentData = mainPrepared->environment;

    m_referenceFrame.viewProjection = viewProjection;
    m_referenceFrame.cameraPosition = packet.camera.position;
    m_referenceFrame.preExposure = frame.preExposure;
    m_referenceFrame.frameSlot = frame.frameSlot;
    m_referenceFrame.view = packet.renderDebug.forwardOnly ? GBufferDebugView::Off : packet.renderDebug.gbufferView;
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
    m_referenceFrame.pathTraced = frame.pathTracing.enabled && !frame.pathTracing.restir;

    // A recording takes the tone mapped image the viewport shows, without the editor's overlays,
    // when its video wants a frame for this moment. Frames of another size than the recording's
    // (the targets not yet resized to the size it fixed) are left out.
    const double videoFrameTime = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    VideoRecorder* const videoRecorder = ActiveVideoRecorder();
    const VkExtent2D videoExtent = m_view.targets->GetOutputExtent();
    const bool recordVideoFrame =
        videoRecorder != nullptr &&
        videoExtent.width == videoRecorder->GetSettings().width &&
        videoExtent.height == videoRecorder->GetSettings().height &&
        VulkanVideoReadback::SupportsFormat(m_view.targets->GetFormat(RenderTargetId::SceneLdr)) &&
        videoRecorder->ClaimFrameAt(videoFrameTime);
    if (recordVideoFrame && !m_videoReadback)
    {
        m_videoReadback = std::make_unique<VulkanVideoReadback>(m_nvrhi->Get(), static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    }
    // The quad recording takes its cameras' images into one canvas, when its video wants this moment
    // and every view is at the size the canvas has room for.
    const std::vector<VulkanVideoReadback::MosaicTile> quadTiles = BuildQuadMosaicTiles(imageIndex);
    const QuadVideoRecording* const quadRecording = ActiveQuadRecording();
    const bool recordQuadFrame =
        quadRecording != nullptr && !quadTiles.empty() &&
        VulkanVideoReadback::SupportsFormat(m_view.targets->GetFormat(RenderTargetId::SceneLdr)) &&
        quadRecording->recorder->ClaimFrameAt(videoFrameTime);
    if (recordQuadFrame && !m_quadReadback)
    {
        m_quadReadback = std::make_unique<VulkanVideoReadback>(m_nvrhi->Get(), static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    }

    // The UI named the render thread's textures by ID; with the swapchain image known, they get the
    // descriptor sets they have now.
    packet.ui.ReplaceTexture(kViewportTextureId, m_view.targets->GetLdrTextureId(imageIndex));
    packet.ui.ReplaceTexture(kSelectionOutlineTextureId, m_view.targets->GetSelectionOutlineTextureId(imageIndex));
    packet.ui.ReplaceTexture(
        kMinimapTextureId,
        m_minimapTextureId);
    for (size_t index = 0; index < kCaptureViewTextureIds.size(); ++index)
    {
        const bool quadView = index < m_captureViews.size() && index != m_photoViewIndex;
        packet.ui.ReplaceTexture(
            kCaptureViewTextureIds[index], quadView ? m_captureViews[index]->targets->GetLdrTextureId(imageIndex) : ImTextureID_Invalid);
    }
    packet.ui.ReplaceTexture(
        kPhotoViewTextureId,
        m_photoViewIndex.has_value() && *m_photoViewIndex < m_captureViews.size()
            ? m_captureViews[*m_photoViewIndex]->targets->GetLdrTextureId(imageIndex)
            : ImTextureID_Invalid);

    m_cpuStages.Mark("FrameSetup");
    m_commandContext->RecordCommandBuffer(imageIndex, [&](VkCommandBuffer commandBuffer)
                                          {
                                              // Every target rests in its initial state between command lists
                                              // (keepInitialState), which NVRHI tracks; the textures made since
                                              // the last frame leave UNDEFINED for it first (Vulkan).
                                              RecordInitialTransitions(frame.commandList);
                                              m_gpuTimer->BeginFrame(frame.commandList, frame.frameSlot);

                                              // The skinned submeshes posed first: every pass after draws them.
                                              m_skinningPass->Record(frame.commandList, frame.frameSlot, shared.skinningDispatches);
                                              m_gpuTimer->Mark("Skinning");

                                              // Ahead of the scene passes, whose material pass samples it, and of
                                              // the probes, whose hits do. It orders itself through its render
                                              // pass dependencies and never goes through the layout tracker (see
                                              // VulkanShadowPass).
                                              m_view.shadowPass->Record(
                                                  frame.commandList,
                                                  frame.frameBindingSet,
                                                  shared.shadowDrawItems,
                                                  mainPrepared->shadowPlan.has_value() ? &*mainPrepared->shadowPlan : nullptr,
                                                  m_gpuTimer.get(),
                                                  m_parallelRecorder.get());
                                              m_cpuStages.Mark("RecordShadows");
                                              // The same, for the local lights' atlas.
                                              m_localShadowPass->Record(frame.commandList, frame.frameBindingSet, shared.shadowDrawItems, shared.localShadowTiles);
                                              m_gpuTimer->Mark("LocalShadows");
                                              m_cpuStages.Mark("RecordLocalShadows");

                                              // Ahead of the scene passes, whose fragment shaders sample the
                                              // LUTs; it orders itself with its own barriers (see
                                              // VulkanAtmosphere).
                                              m_atmosphere->Record(
                                                  commandBuffer,
                                                  frame.commandList,
                                                  frame.frameBindingSet,
                                                  environmentMode == EnvironmentMode::Atmosphere ? &atmosphereParameters : nullptr,
                                                  frame.frameSlot,
                                                  environmentData.cloudLayer.w > 0.0f ? &environmentData.cloudLife : nullptr);
                                              m_gpuTimer->Mark("Atmosphere");
                                              // The viewport's aerial perspective and clouds, marched and
                                              // resolved, after the LUTs and the noise they read and before the
                                              // sky pass composites them.
                                              m_atmosphere->RecordView(
                                                  commandBuffer,
                                                  *m_view.atmosphere,
                                                  frame.commandList,
                                                  frame.frameBindingSet,
                                                  environmentMode == EnvironmentMode::Atmosphere,
                                                  m_gpuTimer.get());
                                              m_gpuTimer->Mark("CloudResolve");
                                              // After the atmosphere, whose sky-view LUT the capture samples.
                                              m_environmentProbe->Record(
                                                  commandBuffer,
                                                  frame.commandList,
                                                  frame.frameBindingSet,
                                                  environmentMode != EnvironmentMode::None,
                                                  environmentData);
                                              m_gpuTimer->Mark("EnvironmentProbe");
                                              // The ray materials, when content changed, and the barrier that
                                              // makes the ray scene visible to every trace after it.
                                              m_rayScene->Record(frame.commandList, frame.frameSlot, frame.hardwareRays);
                                              m_gpuTimer->Mark("RayScene");
                                              // The probes trace the ray scene and must be current before
                                              // any surface samples them.
                                              m_ddgi->Record(
                                                  frame.commandList,
                                                  frame.frameBindingSet,
                                                  m_rayScene->GetBindingSet(frame.frameSlot),
                                                  frame.frameSlot,
                                                  m_ddgiFrameIndex++,
                                                  ddgiHysteresis,
                                                  ddgiLightingEpoch,
                                                  ddgiGeometryEpoch,
                                                  frame.hardwareRays);
                                              m_gpuTimer->Mark("Ddgi");
                                              m_cpuStages.Mark("RecordPrePasses");

                                              // The capture views, each whole: its shadows, its atmosphere and
                                              // clouds, its scene passes, and its image left shader-read for the
                                              // canvas copy and the window's preview. Timed as one section each.
                                              for (const std::unique_ptr<PreparedView>& prepared : capturePrepared)
                                              {
                                                  VulkanSceneView& view = *prepared->view;
                                                  view.shadowPass->Record(
                                                      prepared->frame.commandList,
                                                      prepared->frame.frameBindingSet,
                                                      shared.shadowDrawItems,
                                                      prepared->shadowPlan.has_value() ? &*prepared->shadowPlan : nullptr,
                                                      nullptr,
                                                      m_parallelRecorder.get());
                                                  m_atmosphere->RecordView(
                                                      commandBuffer,
                                                      *view.atmosphere,
                                                      prepared->frame.commandList,
                                                      prepared->frame.frameBindingSet,
                                                      environmentMode == EnvironmentMode::Atmosphere);
                                                  RecordScenePasses(commandBuffer, view, prepared->frame, prepared->passOrder, nullptr);
                                                  static constexpr std::array<RenderTargetId, 1> kCaptureReads = {RenderTargetId::SceneLdr};
                                                  RenderPassIo captureIo{};
                                                  captureIo.reads = kCaptureReads;
                                                  RecordTransitions(view, captureIo, prepared->frame);
                                                  m_gpuTimer->Mark("CaptureView");
                                              }
                                              if (recordQuadFrame)
                                              {
                                                  m_quadReadback->RecordMosaicCopy(
                                                      frame.commandList,
                                                      frame.frameSlot,
                                                      quadTiles,
                                                      m_view.targets->GetFormat(RenderTargetId::SceneLdr),
                                                      ToVkExtent(RenderExtent{quadRecording->mosaic.width, quadRecording->mosaic.height}),
                                                      videoFrameTime);
                                              }
                                              m_cpuStages.Mark("RecordCaptureViews");

                                              RecordScenePasses(commandBuffer, m_view, frame, mainPrepared->passOrder, m_gpuTimer.get());
                                              m_cpuStages.Mark("RecordScenePasses");

                                              // ImGui samples the tone mapped image in the editor pass, which is
                                              // not an IScenePass because it writes the swapchain rather than a
                                              // scene target.
                                              static constexpr std::array<RenderTargetId, 2> kImGuiReads = {
                                                  RenderTargetId::SceneLdr,
                                                  RenderTargetId::SelectionOutline};
                                              RenderPassIo imguiIo{};
                                              imguiIo.reads = kImGuiReads;
                                              RecordTransitions(m_view, imguiIo, frame);

                                              RecordEditorLayer(frame.commandList, imageIndex, frame.frameSlot, packet.ui.GetDrawData());
                                              m_gpuTimer->Mark("ImGui");

                                              if (recordVideoFrame)
                                              {
                                                  // SceneLdr is indexed by swapchain image; the ImGui pass left it
                                                  // shader-read, as CaptureViewport expects to find it.
                                                  const uint32_t ldrIndex = m_view.targets->ResolveIndex(RenderTargetId::SceneLdr, imageIndex, 0);
                                                  m_videoReadback->RecordCopy(
                                                      frame.commandList,
                                                      frame.frameSlot,
                                                      m_view.targets->GetTexture(RenderTargetId::SceneLdr, ldrIndex),
                                                      m_view.targets->GetFormat(RenderTargetId::SceneLdr),
                                                      videoExtent,
                                                      videoFrameTime);
                                              }
                                          });
    m_cpuStages.Mark("RecordRest");
    m_commandContext->Submit(imageIndex);
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
        if (!m_frameTimesChecked)
        {
            m_frameTimesChecked = true;
            if (const char* path = std::getenv("MINIENGINE_FRAME_TIMES"); path != nullptr && path[0] != 0)
            {
                m_frameTimesFile = std::fopen(path, "w");
            }
        }
        if (m_frameTimesFile != nullptr)
        {
            const long long now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            std::fprintf(m_frameTimesFile, "%lld %.3f %.3f %.3f\n", now, frameMs - waitMs, waitMs, m_gpuTimer ? m_gpuTimer->GetLastFrameMs() : 0.0);
        }
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

    const SwapchainStatus presentResult = m_commandContext->Present(imageIndex);
    m_cpuStages.Mark("Present");
    if (acquireResult != SwapchainStatus::Ok || presentResult != SwapchainStatus::Ok)
    {
        // The main thread rebuilds it before its next frame.
        m_swapchainOutOfDate = true;
    }
    PublishFeedback(packet);
}

EnvironmentUniformData VulkanRenderer::BuildViewEnvironment(
    const SharedFrameState& shared,
    const RenderFramePacket& packet,
    const glm::vec3& cameraPosition,
    uint32_t taaFrameIndex) const
{
    EnvironmentUniformData environmentData = BuildEnvironmentUniformData(
        shared.environmentMode,
        packet.environment,
        shared.atmosphereParameters,
        shared.sun,
        cameraPosition,
        shared.environmentMode == EnvironmentMode::Hdri ? &m_environmentMapSh : nullptr);
    // The clouds' march jitter steps with the TAA sequence, which averages it; without TAA the
    // index stands still and so does the noise.
    environmentData.cloudParams.w = static_cast<float>(taaFrameIndex % 64u);
    SetCloudMotion(environmentData, packet.environment, m_cloudMotion);
    return environmentData;
}

std::unique_ptr<VulkanRenderer::PreparedView> VulkanRenderer::PrepareView(
    VulkanSceneView& view,
    const Camera& camera,
    const ViewportMatrices& viewportMatrices,
    bool viewport,
    const SharedFrameState& shared,
    RenderFramePacket& packet,
    RenderExtent wholeExtent,
    const SceneCaptureView* capture)
{
    auto prepared = std::make_unique<PreparedView>();
    prepared->view = &view;
    ScenePassFrameContext& frame = prepared->frame;
    const uint32_t imageIndex = shared.imageIndex;
    const std::vector<GpuLightData>& selectedLights = shared.selectedLights;
    const SceneLightSelection& lightSelection = shared.lightSelection;
    const int32_t shadowLightIndex = shared.shadowLightIndex;

    // A quad view renders what the viewport does, but for what only the viewport's own camera has:
    // DLSS (it resolves with the engine's TAA instead), path tracing (it rasterises with the ray
    // traced effects), and the G-buffer debug views. Photo Mode's view has DLSS of its own (its
    // feature, VulkanSceneView::dlssSlot) and, as its capture asks, the offline path tracer.
    const bool photo = capture != nullptr && capture->photo;
    RenderDebugSettings renderDebug = packet.renderDebug;
    RenderCapabilities capabilities = shared.capabilities;
    if (!viewport)
    {
        renderDebug.taa = renderDebug.taa || capabilities.dlss;
        renderDebug.pathTracing.enabled = false;
        renderDebug.gbufferView = GBufferDebugView::Off;
        capabilities.dlss = false;
        capabilities.dlssRayReconstruction = false;
        if (photo)
        {
            renderDebug.taa = true;
            renderDebug.dlssMode = capture->dlssMode;
            renderDebug.dlssPreset = capture->dlssPreset;
            renderDebug.dlssRayReconstruction = capture->dlssRayReconstruction;
            capabilities.dlss = view.dlssActive && m_dlss->HasFeature(view.dlssSlot);
            capabilities.dlssRayReconstruction = capabilities.dlss && m_dlss->HasRayReconstruction(view.dlssSlot);
            if (capture->offlinePathTracing)
            {
                renderDebug.hardwareRayTracing = true;
                renderDebug.pathTracing.enabled = true;
                renderDebug.pathTracing.offline.enabled = true;
                renderDebug.pathTracing.offline.samplesPerPixel = static_cast<int>(capture->samplesPerPixel);
                renderDebug.pathTracing.offline.targetSamples = static_cast<int>(std::max(capture->targetSamples, 1u));
            }
        }
    }
    // The settings this view's path tracing compares to tell a still image, before the offline mode
    // expands them; then its switches stand in for the path tracer's own (EffectivePathTracing).
    const RenderDebugSettings stillnessSettings = renderDebug;
    renderDebug.pathTracing = EffectivePathTracing(renderDebug.pathTracing);
    const RenderFeatures features = ResolveRenderFeatures(renderDebug, capabilities);
    const bool dlssEnabled = capabilities.dlss;
    const VkExtent2D extent = view.targets->GetExtent();
    const VkExtent2D outputExtent = view.targets->GetOutputExtent();

    ShadowUniformData shadowData{};
    std::optional<ShadowCascades> shadowCascades;
    // The Khronos reference view draws no shadows, as the Sample Viewer does not; the caster stays
    // the sun for the sky and exposure.
    if (shadowLightIndex >= 0 && !renderDebug.khronosReference)
    {
        ShadowCameraInput shadowCamera{};
        shadowCamera.view = viewportMatrices.view;
        shadowCamera.verticalFovRadians = glm::radians(camera.fovDegrees);
        // A tile's camera keeps the whole photo's lens; its cascades cover the whole photo's frustum,
        // the same for every tile.
        shadowCamera.aspect = wholeExtent.IsValid()
                                  ? static_cast<float>(wholeExtent.width) / static_cast<float>(wholeExtent.height)
                                  : static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u));
        shadowCamera.nearPlane = camera.nearPlane;
        shadowCamera.farPlane = camera.farPlane;
        ShadowCascadeSettings shadowSettings{};
        shadowSettings.resolution = view.shadowPass->GetResolution();
        shadowSettings.maxDistance = std::clamp(renderDebug.shadowDistance, 10.0f, 5000.0f);
        shadowCascades = BuildShadowCascades(
            shadowCamera,
            glm::vec3(selectedLights[shadowLightIndex].directionAndType),
            shadowSettings);

        shadowData.params = glm::vec4(
            static_cast<float>(shadowLightIndex),
            1.0f / static_cast<float>(view.shadowPass->GetResolution()),
            0.0f,
            0.0f);
    }
    prepared->shadowPlan = view.shadowPass->Plan(shadowCascades.has_value() ? &*shadowCascades : nullptr, shared.shadowCasterKey);
    if (prepared->shadowPlan.has_value())
    {
        for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
        {
            shadowData.cascadeViewProjection[cascade] = prepared->shadowPlan->held[cascade].viewProjection;
            shadowData.cascadeSplits[cascade] = prepared->shadowPlan->held[cascade].splitFar;
            shadowData.cascadeTexelSizes[cascade] = prepared->shadowPlan->held[cascade].texelWorldSize;
        }
    }

    prepared->environment = BuildViewEnvironment(shared, packet, camera.position, view.taaFrameIndex);
    const EnvironmentUniformData& environmentData = prepared->environment;
    const glm::mat4 viewProjection = viewportMatrices.renderProjection * viewportMatrices.view;
    const MotionFrame motion = view.motionHistory.Advance(viewProjection, shared.motionKeys, shared.models);

    // TAA jitters what the GPU rasterises, and only that: the editor's matrices and the motion
    // history keep the plain projection, and the camera block carries the plain view-projection for
    // the motion vectors. The forward-only order has no motion vectors, so it never jitters. DLSS,
    // when it resolves, always does, through as many phases as it upscales (TaaJitterPhaseCount).
    const bool taaEnabled = dlssEnabled || (renderDebug.taa && features.taa);
    ViewportMatrices renderMatrices = viewportMatrices;
    glm::vec2 jitterPixels(0.0f);
    if (taaEnabled)
    {
        const uint32_t phases = TaaJitterPhaseCount(
            glm::uvec2(extent.width, extent.height),
            glm::uvec2(outputExtent.width, outputExtent.height));
        jitterPixels = TaaJitterPixels(view.taaFrameIndex++, phases);
        renderMatrices.renderProjection = JitterProjection(
            renderMatrices.renderProjection,
            jitterPixels,
            glm::uvec2(extent.width, extent.height));
    }

    // The selection puts every directional light first, so the local lights the grid bins are the
    // tail of selectedLights, and the grid's indices point into the same array the shader reads.
    const bool clusteredLighting = renderDebug.clusteredLighting;
    LightClusterGrid lightClusters;
    if (clusteredLighting)
    {
        std::vector<LightClusterSphere> lightSpheres;
        lightSpheres.reserve(selectedLights.size() - shared.directionalLightCount);
        for (uint32_t index = shared.directionalLightCount; index < static_cast<uint32_t>(selectedLights.size()); ++index)
        {
            const glm::vec4& positionAndRange = selectedLights[index].positionAndRange;
            lightSpheres.push_back(LightClusterSphere{glm::vec3(positionAndRange), positionAndRange.w, index});
        }
        LightClusterCamera clusterCamera{};
        clusterCamera.view = viewportMatrices.view;
        // The jittered projection: the shader finds a pixel's cluster through the one it rasterised
        // with, and the binning must agree with it.
        clusterCamera.projection = renderMatrices.renderProjection;
        clusterCamera.nearPlane = camera.nearPlane;
        clusterCamera.farPlane = camera.farPlane;
        lightClusters = BuildLightClusters(clusterCamera, lightSpheres, kLightClusterIndexCapacity);
    }
    if (viewport)
    {
        ReportDroppedClusterLights(lightClusters.droppedCount);
    }
    LightUpload lightUpload{};
    lightUpload.lights = selectedLights;
    lightUpload.directionalCount = shared.directionalLightCount;
    lightUpload.clusters = clusteredLighting ? &lightClusters : nullptr;
    lightUpload.clustered = clusteredLighting;
    lightUpload.shadowTiles = shared.gpuShadowTiles;

    if ((viewport || photo) && features.plainPathTracing)
    {
        SyncPathTraceHistoryPrecision(view, features.offlinePathTracing);
    }
    // The forward-shaded surfaces path traced as well (the viewport's path tracing only), from images
    // made before the camera block says so.
    const bool pathTraceLayer = (viewport || photo) && features.pathTracing && renderDebug.pathTracing.forwardSurfaces &&
                                PreparePathTraceLayer(view, renderDebug.pathTracing.forwardSurfacesHalfResolution);
    const uint32_t pathTraceLayerShift = pathTraceLayer ? view.pathTracePass->GetLayerShift() : 0u;

    // This frame's EV, already adapted by UpdateAutoExposure, so every writer and reader of the
    // HDR target agrees on one pre-exposure.
    const float preExposure = PreExposureFromEv100(camera.exposureEv100);
    view.uniformBuffer->Update(
        imageIndex,
        renderMatrices,
        camera.position,
        lightSelection.ambientLuminance,
        lightSelection.usesFallbackAmbient,
        lightSelection.ambientGradient,
        lightUpload,
        shadowData,
        motion.previousViewProjection,
        motion.previousModels,
        shared.drawSlots,
        environmentData,
        viewProjection,
        // The Sample Viewer does not filter roughness, so the Khronos reference view does not either.
        renderDebug.specularAntiAliasing && features.specularAntiAliasing,
        preExposure,
        shared.ddgiData,
        dlssEnabled ? UpscaleTextureMipBias(
                          glm::uvec2(extent.width, extent.height),
                          glm::uvec2(outputExtent.width, outputExtent.height))
                    : 0.0f,
        pathTraceLayer,
        pathTraceLayerShift);
    // Culled against the jittered projection, the one the GPU rasterises with.
    std::vector<VulkanDrawItem>& drawItems = prepared->drawItems;
    drawItems = BuildDrawItems(imageIndex, shared.models, renderMatrices.renderProjection * renderMatrices.view, viewportMatrices.view);
    // The deferred decals leave the Blend tail for the geometry pass, in the same back to front
    // order. The forward-only order has no G-buffer, and a device without independent blending no
    // decal pipelines, so there they stay Blend items.
    if (!renderDebug.forwardOnly && m_decalPipelines)
    {
        const auto decals = std::stable_partition(
            drawItems.begin(),
            drawItems.end(),
            [](const VulkanDrawItem& item)
            {
                return !item.decal;
            });
        prepared->decalDrawItems.assign(decals, drawItems.end());
        drawItems.erase(decals, drawItems.end());
    }

    // The anime characters' draws for the toon passes: opaque before transparent, each in Unity's
    // render queue order. Their transparent draws are the toon passes' alone; the opaque ones stay
    // among drawItems for the geometry pass.
    std::vector<VulkanDrawItem>& toonDrawItems = prepared->toonDrawItems;
    for (const VulkanDrawItem& item : drawItems)
    {
        if (item.toon != nullptr)
        {
            toonDrawItems.push_back(item);
        }
    }
    if (!toonDrawItems.empty())
    {
        std::stable_sort(
            toonDrawItems.begin(),
            toonDrawItems.end(),
            [](const VulkanDrawItem& a, const VulkanDrawItem& b)
            {
                const bool aTransparent = a.toon->Has(kToonFeatureTransparent);
                const bool bTransparent = b.toon->Has(kToonFeatureTransparent);
                if (aTransparent != bTransparent)
                {
                    return !aTransparent;
                }
                return a.toon->RenderQueue() < b.toon->RenderQueue();
            });
        drawItems.erase(
            std::remove_if(
                drawItems.begin(),
                drawItems.end(),
                [](const VulkanDrawItem& item)
                {
                    return item.toon != nullptr && item.pipelineKey.alphaMode == MaterialAlphaMode::Blend;
                }),
            drawItems.end());
        // A skinned face's frame follows its head joint: the palette entry takes the bind pose's
        // head to where the animation has it.
        std::vector<glm::mat4> headPoses(toonDrawItems.size(), glm::mat4(1.0f));
        for (size_t index = 0; index < toonDrawItems.size(); ++index)
        {
            const VulkanDrawItem& item = toonDrawItems[index];
            if (item.toonHeadJoint >= 0)
            {
                const std::vector<glm::mat4>* palette = packet.transforms.GetJointPalette(item.entity);
                if (palette != nullptr && static_cast<size_t>(item.toonHeadJoint) < palette->size())
                {
                    headPoses[index] = (*palette)[static_cast<size_t>(item.toonHeadJoint)];
                }
            }
        }
        const uint32_t written = view.toonMaterials->Write(shared.frameSlot, toonDrawItems, headPoses);
        if (written < toonDrawItems.size())
        {
            LOG_WARN("{} toon draws this frame; the toon passes draw the first {}", toonDrawItems.size(), written);
            toonDrawItems.resize(written);
        }
    }

    frame.imageIndex = imageIndex;
    frame.frameSlot = shared.frameSlot;
    frame.recorder = m_parallelRecorder.get();
    frame.hardwareRays = features.hardwareRays;
    frame.extent = extent;
    frame.outputExtent = outputExtent;
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
    frame.decalDrawItems = prepared->decalDrawItems;
    frame.toonDrawItems = toonDrawItems;
    frame.toonExposureScale = std::exp2(std::clamp(renderDebug.toonExposureEv, -8.0f, 8.0f));
    frame.decalPipelines = m_decalPipelines.get();
    for (const VulkanDrawItem& item : drawItems)
    {
        if (item.scatters)
        {
            prepared->scatterDrawItems.push_back(item);
        }
    }
    frame.scatterDrawItems = prepared->scatterDrawItems;
    frame.scatterPipelines = m_scatterPipelines.get();
    frame.frameDescriptorSet = view.uniformBuffer->GetFrameDescriptorSet(imageIndex);
    frame.frameBindingSet = view.uniformBuffer->GetFrameBindingSet(imageIndex);
    frame.drawConstantsSet = m_materialDrawConstants->set;
    frame.commandList = m_commandContext->GetCommandList();
    frame.gbufferDescriptorSet = view.gbufferDescriptors->GetSet(*view.targets, imageIndex, frame.frameSlot);
    frame.gbufferBindingSet = view.gbufferDescriptors->GetBindingSet(*view.targets, imageIndex, frame.frameSlot);
    // The order and the forward filter both derive from this one switch, here, so they cannot
    // disagree. The forward-only order never runs the geometry pass and leaves the G-buffer
    // undefined, so its debug views are forced off rather than trusted to the UI's disabled state.
    prepared->passOrder = BuildScenePassOrder(renderDebug.forwardOnly);
    frame.forwardFilter = renderDebug.forwardOnly ? ForwardDrawFilter::All : ForwardDrawFilter::BlendOnly;
    frame.gbufferView = renderDebug.forwardOnly ? GBufferDebugView::Off : renderDebug.gbufferView;
    frame.preExposure = preExposure;
    // The forward-only order runs neither AO pass, so AO is off there by construction. History
    // advances once per recorded frame; a frame that does not accumulate invalidates the next.
    frame.ao = renderDebug.ao;
    frame.ao.enabled = renderDebug.ao.enabled && features.ao;
    // The ray traced effects, where hardware rays run and the ray scene is ready; the deferred order
    // only, as the passes they replace. Path tracing, where it runs, replaces every ambient term, so
    // the passes that make them stand aside (AO and the probe occlusion here, the GI and the
    // reflections below); the direct lights and their traced shadows stay. As ReSTIR PT
    // (pathTracing.restir) it shades every deferred pixel itself, direct light too, so the traced
    // shadows stand aside as well and the plain path tracer records nothing.
    frame.rayTracing = renderDebug.rayTracing;
    frame.rayTracing.occlusionRays = std::clamp(frame.rayTracing.occlusionRays, 1, 8);
    frame.rayTracing.sunShadows = frame.rayTracing.sunShadows && features.rayTracedSunShadows;
    frame.rayTracing.ambientOcclusion = frame.rayTracing.ambientOcclusion && features.rayTracedAmbientOcclusion;
    frame.rayTracing.probeOcclusion = frame.rayTracing.probeOcclusion && features.probeOcclusion && shared.ddgiData.params.x > 0.0f;
    frame.rayTracing.reflections = frame.rayTracing.reflections && features.rayTracedReflections;
    frame.rayTracing.localShadows = frame.rayTracing.localShadows && features.rayTracedLocalShadows;
    if (features.rayTracedEffects)
    {
        frame.raySet = m_rayScene->GetSet(frame.frameSlot);
        frame.rayTextureSet = m_rayScene->GetTextureSet();
        frame.rayBindingSet = m_rayScene->GetBindingSet(frame.frameSlot);
        frame.rayTextureTable = m_rayScene->GetTextureTable();
    }
    frame.pathTracing = renderDebug.pathTracing;
    frame.pathTracing.enabled = features.pathTracing;
    frame.localLightCount = static_cast<uint32_t>(shared.selectedLights.size()) - shared.directionalLightCount;
    frame.pathTracing.restir = features.restirPt;
    frame.pathTracing.offline.enabled = features.offlinePathTracing;
    // The layer accumulates and denoises by the settings whatever DLSS does: ray reconstruction never
    // sees it.
    frame.pathTraceLayer = pathTraceLayer;
    frame.pathTraceLayerDepthPipelines = m_pathTraceLayerDepthPipelines.get();
    frame.pathTraceLayerSurfacePipelines = m_pathTraceLayerSurfacePipelines.get();
    frame.pathTraceLayerAccumulate = renderDebug.pathTracing.accumulate;
    frame.pathTraceLayerDenoise = renderDebug.pathTracing.denoise;
    frame.pathTraceLayerHistory = view.pathTraceLayerHistory.Advance(pathTraceLayer && frame.pathTraceLayerAccumulate);
    frame.pathTraceLayerHistoryScale = TaaHistoryScale(frame.pathTraceLayerHistory.valid, preExposure, view.pathTraceLayerHistoryPreExposure);
    view.pathTraceLayerHistoryPreExposure = preExposure;
    const RestirPtSettings& restirPt = frame.pathTracing.restirPt;
    if (frame.pathTracing.restir)
    {
        if (view.restirPtPass->Prepare(*view.targets))
        {
            view.restirPtHistory.Reset();
            m_restirPtAccumulatedFrames = 0;
        }
    }
    frame.restirPtHistory = view.restirPtHistory.Advance(frame.pathTracing.restir && (restirPt.temporalReuse || restirPt.spatialReuse));
    frame.previousCameraPosition = view.previousCameraPosition;
    view.previousCameraPosition = camera.position;
    if (viewport)
    {
        // ReSTIR PT's accumulate mode averages while the camera holds still; any change starts it
        // again, as do the scene's geometry and lighting changes (DDGI's epochs count them).
        const glm::mat4 accumulationView = viewportMatrices.renderProjection * viewportMatrices.view;
        const uint64_t accumulationEpochs = (static_cast<uint64_t>(m_ddgiGeometryEpoch) << 32) | m_ddgiLighting.Epoch();
        if (!frame.pathTracing.restir || !restirPt.accumulate || accumulationView != m_restirPtAccumulationView ||
            accumulationEpochs != m_restirPtAccumulationEpochs)
        {
            m_restirPtAccumulatedFrames = 0;
        }
        m_restirPtAccumulationView = accumulationView;
        m_restirPtAccumulationEpochs = accumulationEpochs;
        frame.restirPtAccumulatedFrames = m_restirPtAccumulatedFrames;
        if (frame.pathTracing.restir && restirPt.accumulate)
        {
            ++m_restirPtAccumulatedFrames;
        }
    }
    frame.aoHistory = view.aoHistory.Advance((frame.ao.enabled || frame.rayTracing.probeOcclusion) && frame.ao.temporalFilter);
    // The one-bounce indirect diffuse, likewise only in the deferred order; the Khronos reference
    // view has none, as the Sample Viewer.
    frame.gi = renderDebug.gi;
    frame.gi.enabled = renderDebug.gi.enabled && features.gi;
    frame.giHistory = view.giHistory.Advance(frame.gi.enabled && frame.gi.temporalFilter);
    frame.frameIndex = view.aoFrameIndex++;
    frame.gpuTimer = viewport ? m_gpuTimer.get() : nullptr;
    frame.taaEnabled = taaEnabled;
    frame.bloom = renderDebug.bloom;
    // The Khronos reference view renders what the Sample Viewer does: no glare or bloom (nor AO or
    // SSR, above and below).
    frame.khronosReference = renderDebug.khronosReference;
    frame.toneMapper = renderDebug.toneMapper;
    frame.bloom.enabled = renderDebug.bloom.enabled && features.bloom;

    frame.whiteBalance = shared.whiteBalance;
    frame.hdrOutput = m_swapchain->IsHdr();
    frame.hdrPeakNits = std::clamp(renderDebug.hdrPeakNits, 250.0f, 10000.0f);
    // HDR output shows more of the highlight's brightness directly, so it needs less glare.
    frame.glareImageHeight = wholeExtent.IsValid() ? wholeExtent.height : outputExtent.height;
    frame.glareFNumber = GlareFNumberFromEv100(
        camera.exposureEv100,
        frame.hdrOutput ? frame.hdrPeakNits : kGlareSdrPeakNits);
    frame.taaHistory = view.taaHistory.Advance(taaEnabled);
    frame.taaHistoryScale = TaaHistoryScale(frame.taaHistory.valid, preExposure, view.taaHistoryPreExposure);
    if (dlssEnabled)
    {
        frame.dlss = m_dlss.get();
        frame.dlssSlot = view.dlssSlot;
        frame.jitterPixels = jitterPixels;
        frame.dlssReset = view.dlssResetPending;
        frame.frameTimeMs = packet.deltaSeconds * 1000.0f;
        frame.dlssRayReconstruction = m_dlss->HasRayReconstruction(view.dlssSlot);
        frame.view = viewportMatrices.view;
        frame.projection = viewportMatrices.renderProjection;
        // Ray reconstruction denoises the traced shadow and the paths itself; it wants the raw rays.
        frame.rayTracing.denoise = frame.rayTracing.denoise && features.rayTracedShadowDenoise;
        frame.pathTracing.accumulate = frame.pathTracing.accumulate && features.pathTraceAccumulate;
        frame.pathTracing.denoise = frame.pathTracing.denoise && features.pathTraceDenoise;
        // The raw paths' reflections reproject by their hit distance (its guides); so do the offline
        // mode's accumulated ones, which keep the latest hit distance beside them.
        frame.pathTraceHitDistance = frame.dlssRayReconstruction && frame.pathTracing.reflectionGuides && frame.pathTracing.enabled &&
                                     !frame.pathTracing.restir &&
                                     (frame.pathTracing.offline.enabled || (!frame.pathTracing.accumulate && !frame.pathTracing.denoise));
        view.dlssResetPending = false;
    }
    // The traced shadow accumulates only while its filters run.
    frame.rtShadowHistory = view.rtShadowHistory.Advance(frame.rayTracing.sunShadows && frame.rayTracing.denoise);
    if (viewport || photo)
    {
        UpdatePathTracing(
            view, viewportMatrices, stillnessSettings, viewport, frame, packet, selectedLights, lightSelection.ambientLuminance, environmentData,
            preExposure);
    }
    // Reflections take their colour from TAA's history, so they trace only where it is valid; the
    // forward-only order has no G-buffer to trace from.
    frame.ssr = renderDebug.ssr;
    frame.ssr.enabled = renderDebug.ssr.enabled && features.ssr;
    frame.ssrHistory = view.ssrHistory.Advance(SsrTraces(frame));
    // The history this frame writes carries this frame's pre-exposure.
    view.taaHistoryPreExposure = preExposure;
    frame.physicalSky = shared.environmentMode != EnvironmentMode::None;
    frame.groundPlane = shared.environmentMode == EnvironmentMode::Atmosphere && packet.environment.atmosphere.groundPlane;
    if (viewport)
    {
        // Unjittered, as the editor's other overlays are drawn, so the outline holds still.
        prepared->selectionDrawItems = BuildSelectionDrawItems(packet.selectedEntity, shared.models, viewProjection, camera.position);
        frame.selectionDrawItems = prepared->selectionDrawItems;
        frame.selectionViewProjection = viewProjection;
        // Blender's outline is about a pixel and a half at its UI scale; here in the output's pixels,
        // which the render scale or a fixed resolution makes fewer or more than the screen's.
        frame.selectionOutlineWidth = 1.5f * packet.uiScale * std::clamp(packet.viewportOutputScale, 0.1f, 8.0f);
    }
    return prepared;
}

void VulkanRenderer::PublishFeedback(const RenderFramePacket& frame)
{
    const std::lock_guard lock(m_feedbackMutex);
    m_feedback.serial = frame.serial;
    m_feedback.exposureEv100 = frame.camera.exposureEv100;
    m_feedback.adaptedLongTermEv100 = frame.camera.adaptedLongTermEv100;
    m_feedback.adaptedWhiteKelvin = frame.camera.adaptedWhiteKelvin;
    m_feedback.sceneUploadStatus = m_sceneUploadStatus;
    m_feedback.rayScenePending = m_rayScene->IsBuilding() || !m_rayScene->IsReady();
    if (m_outOfMemoryChange.has_value())
    {
        m_feedback.outOfMemory = m_outOfMemoryChange;
        m_outOfMemoryChange.reset();
    }
    m_feedback.minimapLoaded = m_minimapTextureId != ImTextureID_Invalid;
    // A driver query a few times a second is plenty for streaming, which waits a second between steps.
    if (frame.serial >= m_gpuMemory.serial + 10)
    {
        m_gpuMemory = MeasureGpuMemory(frame);
    }
    m_feedback.gpuMemory = m_gpuMemory;
    m_feedback.pathTracingStatus = m_pathTracingStatus;
    m_feedback.pathTracingProgress = m_pathTracingProgress;
    m_feedback.photoView.reset();
    if (m_photoViewIndex.has_value() && *m_photoViewIndex < m_captureViews.size())
    {
        const VulkanSceneView& photo = *m_captureViews[*m_photoViewIndex];
        PhotoViewReport report;
        report.tile = m_photoTile;
        report.offline = photo.offlineProgress;
        report.dlss = photo.dlssActive ? m_photoDlssMode : DlssMode::Off;
        report.rayReconstruction = photo.dlssActive && m_photoDlssRayReconstruction;
        m_feedback.photoView = report;
    }
}

GpuMemoryReport VulkanRenderer::MeasureGpuMemory(const RenderFramePacket& frame) const
{
    const GpuLocalMemory local = m_nvrhi->QueryLocalMemory();
    const uint64_t targetBytes = m_view.targets ? m_view.targets->GetAllocatedBytes() : 0;
    GpuMemoryReport report;
    report.serial = frame.serial;
    report.budget = local.budget;
    report.worldBytes = VulkanMemoryPool::CommittedBytes();
    // Without the driver's count, what is known: the scene's content and its targets.
    report.usage = local.measured ? local.usage : report.worldBytes + targetBytes;

    // The targets scale with the output's pixels; the ones outside SceneRenderTargets that do too (the
    // bloom chain, TAA's history, the scatter targets, DLSS's own) add about half again.
    static constexpr double kResolutionDependentFactor = 1.5;
    static constexpr uint64_t kMargin = uint64_t{256} << 20;
    double growth = 0.0;
    if (m_view.targets && frame.displayExtent.IsValid())
    {
        const VkExtent2D output = m_view.targets->GetOutputExtent();
        const double outputPixels = std::max(1.0, static_cast<double>(output.width) * output.height);
        const double displayPixels = static_cast<double>(frame.displayExtent.width) * frame.displayExtent.height;
        growth = std::max(0.0, displayPixels / outputPixels - 1.0);
    }
    report.reserve = static_cast<uint64_t>(static_cast<double>(targetBytes) * kResolutionDependentFactor * growth) + kMargin;
    if (m_view.targets)
    {
        const VkExtent2D output = m_view.targets->GetOutputExtent();
        report.viewBytesPerPixel =
            static_cast<double>(targetBytes) * kResolutionDependentFactor / std::max(1.0, static_cast<double>(output.width) * output.height);
    }
    return report;
}

void VulkanRenderer::RunWithRenderIdle(const std::function<void()>& work)
{
    if (m_renderThread)
    {
        m_renderThread->RunExclusive(work);
    }
    else
    {
        work();
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
    m_swapchain = m_nvrhi->CreateSwapchain(GetWindow().GetSDLWindow(), State().renderDebug.hdrOutput);
    m_swapchainHdrRequested = State().renderDebug.hdrOutput;
    // The swapchain's images as NVRHI textures, which ImGui draws into (RecordEditorLayer).
    m_backBuffers.clear();
    m_backBufferFramebuffers.clear();
    for (uint32_t index = 0; index < m_swapchain->GetImageCount(); ++index)
    {
        nvrhi::ITexture* texture = m_swapchain->GetImage(index);
        m_backBufferFramebuffers.push_back(CreateNvrhiFramebuffer(m_nvrhi->Get(), {texture}));
        m_backBuffers.push_back(texture);
    }
    m_commandContext = std::make_unique<VulkanCommandContext>(m_nvrhi->Get(), *m_swapchain);
    if (!State().requestedViewportExtent.IsValid())
    {
        State().requestedViewportExtent = FromVkExtent(m_swapchain->GetExtent());
    }

    // Anything that throws below propagates out of the frame path with m_view.passes left empty
    // (DestroySwapchainResources cleared it), so no half-built target set is ever recordable.
    // SceneRenderTargets does not unwind the images it already created when its constructor
    // throws, so recovering in place here is not possible; failing loudly is the whole handling.
    const VkExtent2D viewportExtent = ToVkExtent(State().requestedViewportExtent);
    const uint32_t swapchainImageCount = static_cast<uint32_t>(m_swapchain->GetImageCount());
    // Rebuild re-creates the images at a new size and image count but never re-runs format
    // selection, so it can only carry the target set across a swapchain recreate while the LDR
    // target's format still matches the swapchain's. A surface format change is rare but real,
    // and it has to reach the LDR target: the tone mapping pass builds its render pass on that
    // format and ImGui samples the image, so a stale one would be a wrong-format viewport. Only
    // a fresh SceneRenderTargets re-runs the selection, so that case is reconstructed outright.
    // With HDR output the LDR target holds display-linear values above UI white, which only a float
    // format keeps; ImGui's HDR shader encodes them for the swapchain. Without it the target is the
    // swapchain's format made sRGB: tone mapping writes linear light and the image encodes it, and
    // ImGui, which draws in sRGB space into a UNORM swapchain, reads those bytes through a UNORM view.
    const VkFormat ldrFormat = m_swapchain->IsHdr() ? VK_FORMAT_R16G16B16A16_SFLOAT : SrgbFormatOf(m_swapchain->GetFormat());
    const bool ldrFormatMatchesSwapchain =
        m_view.targets != nullptr && m_view.targets->GetFormat(RenderTargetId::SceneLdr) == ldrFormat;
    // At the viewport's size: SyncSceneTargets moves the render size to DLSS's before a frame draws.
    if (ldrFormatMatchesSwapchain)
    {
        m_view.targets->Rebuild(viewportExtent, viewportExtent, swapchainImageCount);
    }
    else
    {
        m_view.targets = std::make_unique<SceneRenderTargets>(
            NativePhysicalDevice(),
            NativeDevice(),
            m_nvrhi->Get(),
            ldrFormat,
            viewportExtent,
            viewportExtent,
            swapchainImageCount);
    }
    m_activeDlssMode = DlssMode::Off;
    m_activeDlssPreset = DlssPreset::Default;
    m_activeDlssRayReconstruction = false;
    m_view.dlssResetPending = true;
    // The clouds' targets follow the scene's extent; the descriptor sets built after this name the
    // target, and the device is idle here.
    m_atmosphere->EnsureCloudTarget(*m_view.atmosphere, m_view.targets->GetExtent());

    m_view.ResetHistories();
    m_view.pathTraceAccumulation.Reset();
    CreateScenePasses(m_view);
}

void VulkanRenderer::DestroySwapchainResources()
{
    // Destroying the passes takes their render passes with them, so the pipelines built against
    // them go too; CreateScenePasses rebuilds both together. The target images are released
    // rather than destroyed: the LDR copies are sized by the swapchain image count, and every
    // ImGui texture binding must be released before ImGui's descriptor pool goes away. Nothing is
    // recordable until CreateSwapchainResources repopulates the pass list, and a released image
    // is undefined again, so the tracker goes back to square one with it.
    // The capture views go whole: their LDR copies follow the swapchain's image count and are bound
    // in ImGui's pool too. The frames that name them make them again.
    m_captureViews.clear();
    m_view.ClearPasses();
    m_forwardPipelines.reset();
    m_scatterPipelines.reset();
    m_geometryPipelines.reset();
    m_decalPipelines.reset();
    m_pathTraceLayerDepthPipelines.reset();
    m_pathTraceLayerSurfacePipelines.reset();
    if (m_view.targets)
    {
        m_view.targets->ReleaseImages();
    }
    m_commandContext.reset();
    m_windowCaptureStaging = nullptr;
    m_backBufferFramebuffers.clear();
    m_backBuffers.clear();
    m_swapchain.reset();
}

void VulkanRenderer::CreateDeviceResources()
{
    // All three live as long as the logical device. The frame and material descriptor set
    // layouts are fixed by the shader, so hoisting them out of VulkanUniformBuffer lets a scene
    // reload rebuild descriptor sets without invalidating the pipelines. The pipeline cache
    // outliving every VulkanPipelineSet is what lets a rebuild reuse the driver's earlier shader
    // compilation.
    m_frameSetLayout = std::make_unique<VulkanFrameDescriptorSetLayout>(m_nvrhi->Get());
    m_materialSetLayout = std::make_unique<VulkanMaterialDescriptorSetLayout>(m_nvrhi->Get());
    m_materialSets = std::make_unique<VulkanMaterialSetCache>(m_nvrhi->Get(), m_materialSetLayout->Get());
    m_materialDrawConstants = std::make_unique<MaterialDrawConstants>(m_nvrhi->Get());
    m_uploadPool = std::make_unique<GpuUploadPool>(m_nvrhi->Get());

    m_view.shadowPass = std::make_unique<VulkanShadowPass>(
        m_nvrhi->Get(), m_frameSetLayout->Get(), m_materialSetLayout->Get(), kShadowMapResolution);
    m_localShadowPass = std::make_unique<VulkanLocalShadowPass>(m_nvrhi->Get(), m_frameSetLayout->Get(), m_materialSetLayout->Get());
    m_transmissionImage = std::make_unique<VulkanTransmissionImage>(NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get());

    m_atmosphere = std::make_unique<VulkanAtmosphere>(NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(), m_frameSetLayout->Get());
    m_view.atmosphere = m_atmosphere->CreateView();
    // The scene as the DDGI probe rays trace it, one instance buffer per frame in flight. Its texture
    // table names a white texture where no material's is, and every material sampler is in its sets.
    std::vector<nvrhi::ISampler*> raySamplerTable;
    for (uint32_t index = 0; index < VulkanSamplerCache::kSamplerCount; ++index)
    {
        raySamplerTable.push_back(m_samplerCache->Get(VulkanSamplerCache::SamplerAt(index)));
    }
    {
        VulkanUploadBatch rayUploadBatch(m_nvrhi->Get());
        m_rayDefaultTexture = std::make_unique<VulkanTexture>(
            NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(), CreateSolidTexture(255, 255, 255, 255), rayUploadBatch, VulkanTextureFormat::LinearData);
        rayUploadBatch.Flush();
    }
    const TextureDescriptorBinding rayDefaultTexture =
        BindTexture(m_rayDefaultTexture->GetImageView(), m_rayDefaultTexture->GetNvrhiTexture(), m_samplerCache->Get(TextureSampler{}));
    m_rayScene = std::make_unique<VulkanRayScene>(
        NativePhysicalDevice(),
        NativeDevice(),
        m_nvrhi->Get(),
        m_nvrhi->GetNvrhiVulkan(),
        static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight),
        m_nvrhi->SupportsRayQuery(),
        rayDefaultTexture,
        std::move(raySamplerTable),
        m_nvrhi->SupportsUpdateUnusedWhilePending(),
        m_nvrhi->HasLargeHostVisibleDeviceMemory());
    m_rayScene->SetRetire([this](std::function<void()> release)
                          {
                              Retire(std::move(release));
                          });
    m_skinningPass = std::make_unique<VulkanSkinningPass>(m_nvrhi->Get(), static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    m_ddgi = std::make_unique<VulkanDdgi>(
        m_nvrhi->Get(),
        m_frameSetLayout->Get(),
        m_rayScene->GetNvrhiSetLayout(),
        static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight),
        m_rayScene->HasHardwareRayTracing());
    m_environmentProbe = std::make_unique<VulkanEnvironmentProbe>(
        NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(), m_frameSetLayout->Get());

    // Set 0 binding 6 must name a valid image even when no HDRI is loaded.
    VulkanUploadBatch uploadBatch(m_nvrhi->Get());
    FloatTextureData black{};
    black.width = 1;
    black.height = 1;
    black.pixels = {0.0f, 0.0f, 0.0f, 1.0f};
    m_defaultEnvironmentMap = std::make_unique<VulkanTexture>(
        NativePhysicalDevice(),
        NativeDevice(),
        m_nvrhi->Get(),
        black,
        uploadBatch);
    // The DFG table, one mip, RGBA32F; the shader clamps its lookups to texel centres, so the
    // equirectangular sampler's u repeat never shows.
    m_environmentBrdfLut = std::make_unique<VulkanTexture>(
        NativePhysicalDevice(),
        NativeDevice(),
        m_nvrhi->Get(),
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
        NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(), ltcTexture(kLtcInverseMatrices), uploadBatch);
    m_ltcAmplitudes = std::make_unique<VulkanTexture>(
        NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(), ltcTexture(kLtcAmplitudes), uploadBatch);
    uploadBatch.Flush();

    m_gpuTimer = std::make_unique<VulkanGpuTimer>(m_nvrhi->Get(), static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    // Secondary command buffers are Vulkan's (and no pass records into them at present).
    if (State().parallelRecording && m_nvrhi->GetVulkanDevice() != nullptr)
    {
        m_parallelRecorder = std::make_unique<VulkanParallelRecorder>(
            NativeDevice(),
            m_nvrhi->GetVulkanDevice()->GetQueueFamilies().graphicsFamily.value(),
            static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    }
}

void VulkanRenderer::LogFrameTimings() const
{
    m_renderThread->RunExclusive([this]()
                                 {
                                     LogFrameTimingsNow();
                                 });
}

void VulkanRenderer::LogFrameTimingsNow() const
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
    LOG_INFO(
        "Main thread: {:.2f} ms a frame ({} frames), render work on {}",
        average(m_mainFrameMs),
        m_mainFrameMs.size(),
        m_renderThread && m_renderThread->GetMode() == RenderThread::Mode::Threaded ? "the render thread" : "the main thread");
    for (const CpuStageTimer::Stage& stage : m_mainStages.GetStages())
    {
        LOG_INFO("  Main {:<19} {:7.3f} ms", stage.name, stage.averageMs);
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
    m_renderThread->RunExclusive([&]()
                                 {
                                     CaptureViewportNow(path);
                                 });
}

PhotoViewPicture VulkanRenderer::ReadPhotoView()
{
    return m_renderThread->RunExclusive([&]()
                                        {
                                            return ReadPhotoViewNow();
                                        });
}

PhotoViewPicture VulkanRenderer::ReadPhotoViewNow()
{
    if (!m_photoViewIndex.has_value() || *m_photoViewIndex >= m_captureViews.size())
    {
        throw std::runtime_error(m_photoViewError.empty() ? "no frame drew the photo" : m_photoViewError);
    }
    if (!m_lastRecordedImageIndex.has_value())
    {
        throw std::runtime_error("No frame has been drawn to capture");
    }
    m_nvrhi->Get()->waitForIdle();

    const VulkanSceneView& view = *m_captureViews[*m_photoViewIndex];
    ImageCaptureRequest request{};
    request.device = m_nvrhi->Get();
    // SceneLdr is indexed by swapchain image; the frame slot is ignored for it.
    const uint32_t index = view.targets->ResolveIndex(RenderTargetId::SceneLdr, *m_lastRecordedImageIndex, 0);
    request.texture = view.targets->GetTexture(RenderTargetId::SceneLdr, index);
    request.format = view.targets->GetFormat(RenderTargetId::SceneLdr);
    request.extent = view.targets->GetOutputExtent();
    PhotoViewPicture picture;
    picture.rgba = ReadImageRgba8(request);
    picture.width = request.extent.width;
    picture.height = request.extent.height;
    picture.exposureEv100 = view.exposureEv100.value_or(State().camera.exposureEv100);
    return picture;
}

void VulkanRenderer::CaptureDdgiReference(const DdgiReferenceRequest& reference)
{
    m_renderThread->RunExclusive([&]()
                                 {
                                     CaptureDdgiReferenceNow(reference);
                                 });
}

void VulkanRenderer::CaptureViewportNow(const std::filesystem::path& path)
{
    if (!m_lastRecordedImageIndex.has_value() || !m_view.targets)
    {
        throw std::runtime_error("No frame has been drawn to capture");
    }
    m_nvrhi->Get()->waitForIdle();

    ImageCaptureRequest request{};
    request.device = m_nvrhi->Get();
    // SceneLdr is indexed by swapchain image; the frame slot is ignored for it.
    const uint32_t index = m_view.targets->ResolveIndex(RenderTargetId::SceneLdr, *m_lastRecordedImageIndex, 0);
    request.texture = m_view.targets->GetTexture(RenderTargetId::SceneLdr, index);
    request.format = m_view.targets->GetFormat(RenderTargetId::SceneLdr);
    request.extent = m_view.targets->GetOutputExtent();
    CaptureImageToPng(request, path);
    if (const char* windowCapture = std::getenv("MINIENGINE_CAPTURE_WINDOW"); windowCapture != nullptr && windowCapture[0] != 0 && m_windowCaptureStaging)
    {
        const nvrhi::TextureDesc& desc = m_windowCaptureStaging->getDesc();
        size_t rowPitch = 0;
        const auto* mapped = static_cast<const uint8_t*>(
            m_nvrhi->Get()->mapStagingTexture(m_windowCaptureStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        if (mapped != nullptr)
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(desc.width) * desc.height * 4);
            const bool bgra = desc.format == nvrhi::Format::BGRA8_UNORM || desc.format == nvrhi::Format::SBGRA8_UNORM;
            for (uint32_t row = 0; row < desc.height; ++row)
            {
                for (uint32_t x = 0; x < desc.width; ++x)
                {
                    const uint8_t* texel = mapped + row * rowPitch + x * 4;
                    uint8_t* out = &pixels[(static_cast<size_t>(row) * desc.width + x) * 4];
                    out[0] = bgra ? texel[2] : texel[0];
                    out[1] = texel[1];
                    out[2] = bgra ? texel[0] : texel[2];
                    out[3] = 255;
                }
            }
            m_nvrhi->Get()->unmapStagingTexture(m_windowCaptureStaging);
            stbi_write_png(windowCapture, static_cast<int>(desc.width), static_cast<int>(desc.height), 4, pixels.data(), static_cast<int>(desc.width) * 4);
            LOG_INFO("Captured the window to '{}'", windowCapture);
        }
    }
    // The EV the render thread drew the frame with; the main thread's camera trails it by a frame.
    LOG_INFO("Captured the viewport to '{}' at EV100 {:.2f}", path.string(), m_view.exposureEv100.value_or(State().camera.exposureEv100));
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
    m_nvrhi->Get()->waitForIdle();
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

void VulkanRenderer::SubmitQuadVideoFrame(uint32_t frameSlot)
{
    if (!m_quadReadback)
    {
        return;
    }
    if (std::optional<VulkanVideoReadback::Frame> canvas = m_quadReadback->Take(frameSlot))
    {
        SubmitQuadCanvas(*canvas);
    }
}

void VulkanRenderer::SubmitQuadCanvas(VulkanVideoReadback::Frame& canvas)
{
    const QuadVideoRecording* const recording = ActiveQuadRecording();
    if (recording == nullptr || canvas.extent.width != recording->mosaic.width || canvas.extent.height != recording->mosaic.height)
    {
        return;
    }
    // Each camera's name in its picture's top-left corner, a font pixel for every 180 rows.
    for (size_t index = 0; index < recording->mosaic.tiles.size() && index < recording->labels.size(); ++index)
    {
        const VideoMosaicTile& tile = recording->mosaic.tiles[index];
        const uint32_t scale = std::max(2u, tile.height / 180u);
        DrawVideoLabel(
            canvas.frame.pixels,
            canvas.frame.format,
            canvas.extent.width,
            canvas.extent.height,
            tile.x + scale * 3,
            tile.y + scale * 3,
            recording->labels[index],
            scale);
    }
    recording->recorder->Submit(std::move(canvas.frame));
}

void VulkanRenderer::FlushQuadVideoFrames()
{
    if (!m_quadReadback || !m_quadReadback->HasPending())
    {
        return;
    }
    m_nvrhi->Get()->waitForIdle();
    // Oldest first: the recorder writes them in the order it is given them.
    std::vector<VulkanVideoReadback::Frame> canvases;
    for (uint32_t slot = 0; slot < VulkanCommandContext::kMaxFramesInFlight; ++slot)
    {
        if (std::optional<VulkanVideoReadback::Frame> canvas = m_quadReadback->Take(slot))
        {
            canvases.push_back(std::move(*canvas));
        }
    }
    std::sort(canvases.begin(), canvases.end(), [](const auto& left, const auto& right)
              {
                  return left.frame.timeSeconds < right.frame.timeSeconds;
              });
    for (VulkanVideoReadback::Frame& canvas : canvases)
    {
        SubmitQuadCanvas(canvas);
    }
}

std::vector<VulkanVideoReadback::MosaicTile> VulkanRenderer::BuildQuadMosaicTiles(uint32_t imageIndex) const
{
    const QuadVideoRecording* const recording = ActiveQuadRecording();
    // The quad cameras come first; Photo Mode's view may follow them.
    if (recording == nullptr || m_captureViews.size() < recording->mosaic.tiles.size() ||
        (m_photoViewIndex.has_value() && *m_photoViewIndex < recording->mosaic.tiles.size()))
    {
        return {};
    }
    std::vector<VulkanVideoReadback::MosaicTile> tiles;
    for (size_t index = 0; index < recording->mosaic.tiles.size(); ++index)
    {
        const VulkanSceneView& view = *m_captureViews[index];
        const VideoMosaicTile& tile = recording->mosaic.tiles[index];
        const VkExtent2D extent = view.targets->GetOutputExtent();
        if (extent.width != tile.width || extent.height != tile.height)
        {
            return {};
        }
        // SceneLdr is indexed by swapchain image; the frame slot is ignored for it.
        const uint32_t ldrIndex = view.targets->ResolveIndex(RenderTargetId::SceneLdr, imageIndex, 0);
        tiles.push_back(VulkanVideoReadback::MosaicTile{view.targets->GetTexture(RenderTargetId::SceneLdr, ldrIndex), extent, tile.x, tile.y});
    }
    return tiles;
}

VkDevice VulkanRenderer::NativeDevice() const
{
    return m_nvrhi->GetVulkanDevice() != nullptr ? m_nvrhi->GetVulkanDevice()->GetHandle() : VK_NULL_HANDLE;
}

VkPhysicalDevice VulkanRenderer::NativePhysicalDevice() const
{
    return m_nvrhi->GetVulkanDevice() != nullptr ? m_nvrhi->GetVulkanDevice()->GetPhysicalDevice() : VK_NULL_HANDLE;
}

void VulkanRenderer::DestroyDeviceResources()
{
    m_parallelRecorder.reset();
    m_gpuTimer.reset();
    m_environmentMap.reset();
    m_environmentMapPath.clear();
    m_defaultEnvironmentMap.reset();
    m_environmentBrdfLut.reset();
    m_ltcInverseMatrices.reset();
    m_ltcAmplitudes.reset();
    m_environmentProbe.reset();
    m_ddgi.reset();
    m_view.toonMaterials.reset();
    m_skinningPass.reset();
    m_rayScene.reset();
    m_rayDefaultTexture.reset();
    m_view.atmosphere.reset();
    m_atmosphere.reset();
    // Its pipelines were built against the material set layout released below.
    m_view.shadowPass.reset();
    m_localShadowPass.reset();
    m_transmissionImage.reset();
    // The cached material sets come from pools made against the layout; content that named them is gone.
    m_renderSubmeshes.clear();
    m_liveSubmeshes.clear();
    m_materialSets.reset();
    m_materialDrawConstants.reset();
    m_materialSetLayout.reset();
    m_frameSetLayout.reset();
}

// The sampler of the float textures (the environment map and the DFG and LTC tables): linear with
// mips, repeating in u for the longitude wrap and clamping in v at the poles.
nvrhi::ISampler* VulkanRenderer::EquirectangularSampler() const
{
    TextureSampler sampler;
    sampler.wrapT = TextureWrap::ClampToEdge;
    return m_samplerCache->Get(sampler);
}

EnvironmentDescriptorBindings VulkanRenderer::BuildEnvironmentBindings(const VulkanSceneView& view) const
{
    EnvironmentDescriptorBindings bindings{};
    bindings.transmittance = m_atmosphere->GetTransmittanceBinding();
    bindings.skyView = m_atmosphere->GetSkyViewBinding();
    bindings.aerialPerspective = m_atmosphere->GetAerialPerspectiveBinding(*view.atmosphere);
    bindings.cloudShapeNoise = m_atmosphere->GetCloudShapeNoiseBinding();
    bindings.cloudDetailNoise = m_atmosphere->GetCloudDetailNoiseBinding();
    bindings.cloudShadow = m_atmosphere->GetCloudShadowBinding();
    bindings.cloudWeather = m_atmosphere->GetCloudWeatherBinding();
    bindings.cloudTarget = m_atmosphere->GetCloudTargetBinding(*view.atmosphere);
    bindings.irradiance = m_atmosphere->GetIrradianceBuffer();
    bindings.prefiltered = m_environmentProbe->GetPrefilteredBinding();
    nvrhi::ISampler* floatTableSampler = EquirectangularSampler();
    bindings.brdfLut = BindTexture(m_environmentBrdfLut->GetImageView(), m_environmentBrdfLut->GetNvrhiTexture(), floatTableSampler);
    bindings.ltcInverseMatrices = BindTexture(m_ltcInverseMatrices->GetImageView(), m_ltcInverseMatrices->GetNvrhiTexture(), floatTableSampler);
    bindings.ltcAmplitudes = BindTexture(m_ltcAmplitudes->GetImageView(), m_ltcAmplitudes->GetNvrhiTexture(), floatTableSampler);
    bindings.transmission = m_transmissionImage->GetSampledBinding();
    bindings.scatterLight = view.scatterPass->GetLightBinding();
    bindings.scatterDepth = view.scatterPass->GetDepthBinding();
    PathTraceLayerBindings(view, bindings.pathTraceLayerDepth, bindings.pathTraceLayerDiffuse, bindings.pathTraceLayerSpecular);
    bindings.ddgiIrradiance = m_ddgi->GetIrradianceBinding();
    bindings.ddgiVisibility = m_ddgi->GetVisibilityBinding();
    bindings.ddgiProbeStates = m_ddgi->GetProbeStateHandle();
    const VulkanTexture& environmentMap = m_environmentMap ? *m_environmentMap : *m_defaultEnvironmentMap;
    bindings.environmentMap = BindTexture(environmentMap.GetImageView(), environmentMap.GetNvrhiTexture(), floatTableSampler);
    bindings.materialSamplers.reserve(kMaterialSamplerCount);
    for (uint32_t index = 0; index < kMaterialSamplerCount; ++index)
    {
        bindings.materialSamplers.push_back(m_samplerCache->Get(VulkanSamplerCache::SamplerAt(index)));
    }
    return bindings;
}

void VulkanRenderer::PathTraceLayerBindings(
    const VulkanSceneView& view, TextureDescriptorBinding& depth, TextureDescriptorBinding& diffuse, TextureDescriptorBinding& specular) const
{
    if (view.pathTraceLayerPass != nullptr && view.pathTraceLayerPass->IsReady() && view.pathTracePass != nullptr &&
        view.pathTracePass->IsLayerReady())
    {
        depth = view.pathTraceLayerPass->GetDepthBinding();
        diffuse = view.pathTracePass->GetLayerDiffuseBinding();
        specular = view.pathTracePass->GetLayerSpecularBinding();
        return;
    }
    // Never sampled while the camera block says there is no layer (textureParams.y), but named in
    // the layout the real ones rest in.
    const TextureDescriptorBinding placeholder =
        BindTexture(m_environmentBrdfLut->GetImageView(), m_environmentBrdfLut->GetNvrhiTexture(), EquirectangularSampler());
    depth = placeholder;
    diffuse = placeholder;
    specular = placeholder;
}

void VulkanRenderer::SyncPathTraceHistoryPrecision(VulkanSceneView& view, bool fullPrecision)
{
    if (view.pathTracePass == nullptr || !view.pathTracePass->SetFullPrecisionHistory(fullPrecision))
    {
        return;
    }
    // The frames in flight still use the old images; set 0 stops naming the layer's result, which
    // the next path traced frame makes again (and points set 0 back at).
    m_commandContext->WaitForAllFrames();
    view.pathTracePass->ReleaseImages();
    TextureDescriptorBinding depth;
    TextureDescriptorBinding diffuse;
    TextureDescriptorBinding specular;
    PathTraceLayerBindings(view, depth, diffuse, specular);
    view.uniformBuffer->SetPathTraceLayerImages(depth, diffuse, specular);
    view.pathTraceLayerHistory.Reset();
    view.pathTraceHistory.Reset();
    m_view.pathTraceAccumulation.Reset();
}

bool VulkanRenderer::PreparePathTraceLayer(VulkanSceneView& view, bool halfResolution)
{
    VulkanPathTraceLayerPass* layerPass = view.pathTraceLayerPass;
    VulkanPathTracePass* pathTracePass = view.pathTracePass;
    if (layerPass == nullptr || pathTracePass == nullptr || !m_pathTraceLayerDepthPipelines || !m_pathTraceLayerSurfacePipelines)
    {
        return false;
    }
    const uint32_t shift = halfResolution ? 1u : 0u;
    if (pathTracePass->IsLayerReady() && pathTracePass->GetLayerShift() != shift)
    {
        // The frames in flight still sample the layer's result at the old size.
        m_commandContext->WaitForAllFrames();
        pathTracePass->DestroyLayerImages();
    }
    bool made = layerPass->Prepare(*view.targets);
    made = pathTracePass->PrepareLayer(*view.targets, *layerPass, shift) || made;
    if (made)
    {
        // Set 0 names the new images from now on, in the layout they rest in: the frames that may
        // still use the sets finish first, and the images are moved there at once.
        m_commandContext->WaitForAllFrames();
        RunNvrhiCommands([&](nvrhi::ICommandList* commandList)
                         {
                             layerPass->RecordInitialTransition(commandList);
                             pathTracePass->RecordLayerInitialTransition(commandList);
                         });
        TextureDescriptorBinding depth;
        TextureDescriptorBinding diffuse;
        TextureDescriptorBinding specular;
        PathTraceLayerBindings(view, depth, diffuse, specular);
        view.uniformBuffer->SetPathTraceLayerImages(depth, diffuse, specular);
        view.pathTraceLayerHistory.Reset();
    }
    return layerPass->IsReady() && pathTracePass->IsLayerReady();
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

void VulkanRenderer::UpdateMinimapTexture(const std::string& path)
{
    if (path != m_minimapPath)
    {
        ReleaseMinimapTexture();
        m_minimapPath = path;
        if (!path.empty())
        {
            const std::filesystem::path file = EnginePaths::ResolveProjectPath(path);
            try
            {
                VulkanUploadBatch uploadBatch(m_nvrhi->Get());
                // Read as UNORM: only ImGui shows it, and ImGui works on sRGB values as they are.
                m_minimapTexture = std::make_unique<VulkanTexture>(
                    NativePhysicalDevice(),
                    NativeDevice(),
                    m_nvrhi->Get(),
                    file.string(),
                    uploadBatch,
                    VulkanTextureFormat::LinearData);
                uploadBatch.Flush();
                // ImGui samples it with its own linear sampler, which clamps: past the picture's edges the
                // minimap shows the edge's colour (the sea, on a game's radar map) rather than the far side.
                m_minimapTextureId = m_imguiLayer->GetRenderer().AddTexture(m_minimapTexture->GetNvrhiTexture());
                LOG_INFO("Minimap: loaded '{}'", file.string());
            }
            catch (const std::exception& error)
            {
                LOG_WARN("Minimap: could not load '{}': {}", file.string(), error.what());
                m_minimapTexture.reset();
            }
        }
    }
}

void VulkanRenderer::ReleaseMinimapTexture()
{
    if (m_minimapTextureId == ImTextureID_Invalid && m_minimapTexture == nullptr)
    {
        return;
    }
    // Frames in flight may still sample it; a scene change is rare enough to wait for them.
    m_nvrhi->Get()->waitForIdle();
    if (m_minimapTextureId != ImTextureID_Invalid)
    {
        m_imguiLayer->GetRenderer().RemoveTexture(m_minimapTextureId);
        m_minimapTextureId = ImTextureID_Invalid;
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
                VulkanUploadBatch uploadBatch(m_nvrhi->Get());
                auto texture = std::make_unique<VulkanTexture>(
                    NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(), image, uploadBatch);
                uploadBatch.Flush();
                // The frame sets name the old map until rewritten, and may be in use.
                m_commandContext->WaitForAllFrames();
                if (m_view.uniformBuffer)
                {
                    m_view.uniformBuffer->SetEnvironmentMap(BindTexture(texture->GetImageView(), texture->GetNvrhiTexture(), EquirectangularSampler()));
                    for (const std::unique_ptr<VulkanSceneView>& view : m_captureViews)
                    {
                        view->uniformBuffer->SetEnvironmentMap(BindTexture(texture->GetImageView(), texture->GetNvrhiTexture(), EquirectangularSampler()));
                    }
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
    m_pendingEnvironmentMap = RunAsync(
        TaskPriority::Medium,
        [path = wanted]()
        {
            PreparedEnvironmentMap prepared{};
            prepared.image = TextureLoader::LoadRGBA32F(path);
            prepared.sh = ProjectEquirectangular(prepared.image);
            return prepared;
        });
}

void VulkanRenderer::CreateScenePasses(VulkanSceneView& view)
{
    // Passes are built fresh here rather than carried across a swapchain recreate. The tone
    // mapping pass could never survive a swapchain format change anyway, and rebuilding the
    // material pipelines alongside them costs little: the driver keeps its own cache of the shader
    // compilations. What that buys is the
    // disappearance of every construct-or-rebuild branch, and with it the window in which the
    // pass list held passes whose framebuffers referenced destroyed views.
    //
    // A viewport resize does NOT come through here. That path rebuilds only the images and tells
    // every owned pass to follow them; see RebuildViewTargets.
    //
    // The material pipelines are built from the viewport's passes and drawn with by every view:
    // a capture view's render passes have the same attachments, so they are compatible.
    const bool viewport = &view == &m_view;
    view.ClearPasses();
    if (viewport)
    {
        m_forwardPipelines.reset();
        m_scatterPipelines.reset();
        m_geometryPipelines.reset();
        m_decalPipelines.reset();
        m_pathTraceLayerDepthPipelines.reset();
        m_pathTraceLayerSurfacePipelines.reset();
    }
    view.gbufferDescriptors = std::make_unique<VulkanGBufferDescriptors>(NativeDevice(), m_nvrhi->Get(), *view.targets);

    auto geometryPass = std::make_unique<VulkanGeometryPass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get());
    auto forwardPass = std::make_unique<VulkanForwardPass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), ForwardPassPart::OpaqueAndSky);

    MaterialPipelineSetConfig geometryConfig{};
    geometryConfig.fragmentShader = "gbuffer.frag.spv";
    geometryConfig.colorAttachmentCount = VulkanGeometryPass::kColorAttachmentCount;
    geometryConfig.writeAlpha = true;
    geometryConfig.allowBlending = false;
    geometryConfig.depthLessOrEqual = false;

    // Both sets are built here, while the typed pass pointers are in hand: the pipelines depend on
    // nothing but these passes' attachment formats and the device lifetime binding layouts.
    if (viewport)
    {
        m_geometryPipelines = std::make_unique<VulkanPipelineSet>(
            m_nvrhi->Get(),
            geometryPass->GetFramebufferInfo(),
            m_frameSetLayout->Get(),
            m_materialSetLayout->Get(),
            m_materialDrawConstants->layout,
            geometryConfig);
        if (m_nvrhi->SupportsIndependentBlend())
        {
            MaterialPipelineSetConfig decalConfig = geometryConfig;
            decalConfig.allowBlending = true;
            decalConfig.depthLessOrEqual = true;
            decalConfig.decal = true;
            m_decalPipelines = std::make_unique<VulkanPipelineSet>(
                m_nvrhi->Get(),
                geometryPass->GetFramebufferInfo(),
                m_frameSetLayout->Get(),
                m_materialSetLayout->Get(),
                m_materialDrawConstants->layout,
                decalConfig);
        }
        // The default config is the forward shape: triangle.frag, one HDR attachment, RGB writes.
        m_forwardPipelines = std::make_unique<VulkanPipelineSet>(
            m_nvrhi->Get(),
            forwardPass->GetFramebufferInfo(),
            m_frameSetLayout->Get(),
            m_materialSetLayout->Get(),
            m_materialDrawConstants->layout,
            MaterialPipelineSetConfig{});
    }
    // The scatter pre-pass: the forward shader's inputs, its own output (light and draw slot, alpha
    // included), its own depth, no blending.
    auto scatterPass = std::make_unique<VulkanScatterPass>(m_nvrhi->Get(), *view.targets);
    if (viewport)
    {
        MaterialPipelineSetConfig scatterConfig{};
        scatterConfig.writeAlpha = true;
        scatterConfig.allowBlending = false;
        scatterConfig.depthLessOrEqual = false;
        scatterConfig.scatterPrepass = true;
        m_scatterPipelines = std::make_unique<VulkanPipelineSet>(
            m_nvrhi->Get(),
            scatterPass->GetFramebufferInfo(),
            m_frameSetLayout->Get(),
            m_materialSetLayout->Get(),
            m_materialDrawConstants->layout,
            scatterConfig);
    }
    view.scatterPass = scatterPass.get();

    // A rebuilt exposure pass starts with zeroed histograms, which meter as empty, so auto
    // exposure holds its current EV for the kMaxFramesInFlight frames until real ones arrive.
    auto exposurePass = std::make_unique<VulkanExposureHistogramPass>(m_nvrhi->Get(), *view.targets);
    view.exposurePass = exposurePass.get();

    // Construction order does not matter: RecordScenePasses follows BuildScenePassOrder.
    view.passes.push_back(std::move(geometryPass));
    view.passes.push_back(std::make_unique<VulkanRtShadowPass>(NativeDevice(), m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), *m_rayScene));
    auto pathTracePass = std::make_unique<VulkanPathTracePass>(
        NativeDevice(),
        m_nvrhi->Get(),
        *view.targets,
        m_frameSetLayout->Get(),
        *m_rayScene,
        m_atmosphere->GetMultiScatteringBinding());
    view.pathTracePass = pathTracePass.get();
    // The forward-shaded surfaces' layer it traces too: gbuffer.frag twice, against its two passes.
    auto pathTraceLayerPass = std::make_unique<VulkanPathTraceLayerPass>(m_nvrhi->Get(), *view.targets);
    if (viewport && pathTraceLayerPass->IsSupported() && pathTracePass->IsSupported())
    {
        MaterialPipelineSetConfig layerConfig{};
        layerConfig.fragmentShader = "path_trace_layer_depth.frag.spv";
        layerConfig.allowBlending = false;
        layerConfig.depthLessOrEqual = true;
        layerConfig.layerPass = 1;
        m_pathTraceLayerDepthPipelines = std::make_unique<VulkanPipelineSet>(
            m_nvrhi->Get(),
            pathTraceLayerPass->GetDepthFramebufferInfo(),
            m_frameSetLayout->Get(),
            m_materialSetLayout->Get(),
            m_materialDrawConstants->layout,
            layerConfig);
        layerConfig.fragmentShader = "path_trace_layer_surface.frag.spv";
        layerConfig.colorAttachmentCount = VulkanPathTraceLayerPass::kSurfaceColorSlots;
        layerConfig.writeAlpha = true;
        layerConfig.layerPass = 2;
        m_pathTraceLayerSurfacePipelines = std::make_unique<VulkanPipelineSet>(
            m_nvrhi->Get(),
            pathTraceLayerPass->GetSurfaceFramebufferInfo(),
            m_frameSetLayout->Get(),
            m_materialSetLayout->Get(),
            m_materialDrawConstants->layout,
            layerConfig);
    }
    view.pathTraceLayerPass = pathTraceLayerPass.get();
    view.passes.push_back(std::move(pathTraceLayerPass));
    view.passes.push_back(std::move(pathTracePass));
    auto restirPtPass = std::make_unique<VulkanRestirPtPass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), *m_rayScene);
    view.restirPtPass = restirPtPass.get();
    view.passes.push_back(std::move(restirPtPass));
    view.passes.push_back(std::make_unique<VulkanAoTracePass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), *m_rayScene));
    view.passes.push_back(std::make_unique<VulkanAoResolvePass>(NativeDevice(), m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get()));
    view.passes.push_back(std::make_unique<VulkanLightingPass>(
        m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), view.gbufferDescriptors->GetBindingLayout(), *m_rayScene));
    view.passes.push_back(std::make_unique<VulkanGiTracePass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get()));
    view.passes.push_back(std::make_unique<VulkanGiResolvePass>(m_nvrhi->Get(), NativeDevice(), *view.targets, m_frameSetLayout->Get()));
    view.passes.push_back(std::make_unique<VulkanGiCompositePass>(
        m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), view.gbufferDescriptors->GetBindingLayout()));
    view.passes.push_back(std::make_unique<VulkanDdgiDebugPass>(
        m_nvrhi->Get(),
        *view.targets,
        m_frameSetLayout->Get(),
        *m_rayScene));
    view.passes.push_back(std::move(scatterPass));
    view.passes.push_back(std::move(forwardPass));
    if (!view.toonMaterials)
    {
        view.toonMaterials = std::make_unique<VulkanToonMaterials>(m_nvrhi->Get(), static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight));
    }
    view.passes.push_back(std::make_unique<VulkanToonPrepass>(
        m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), m_materialSetLayout->Get(), *view.toonMaterials));
    view.passes.push_back(std::make_unique<VulkanToonPass>(
        m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), m_materialSetLayout->Get(), *view.toonMaterials));
    view.passes.push_back(std::make_unique<VulkanTransmissionCopyPass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), *m_transmissionImage));
    // The opaque half's attachment formats, so the same forward pipelines draw in it.
    view.passes.push_back(std::make_unique<VulkanForwardPass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), ForwardPassPart::Translucent));
    auto taaPass = std::make_unique<VulkanTaaPass>(m_nvrhi->Get(), NativeDevice(), *view.targets, m_frameSetLayout->Get());
    const VulkanTaaPass& taa = *taaPass;
    view.passes.push_back(std::move(taaPass));
    // After TAA in this list, whose order OnTargetsRebuilt follows: the trace names TAA's history
    // images, which TAA recreates first.
    view.passes.push_back(std::make_unique<VulkanSsrTracePass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), taa, *m_rayScene));
    view.passes.push_back(std::make_unique<VulkanSsrResolvePass>(NativeDevice(), m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get()));
    view.passes.push_back(std::make_unique<VulkanBloomPass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get()));
    view.passes.push_back(std::move(exposurePass));
    view.passes.push_back(std::make_unique<VulkanTonemapPass>(m_nvrhi->Get(), *view.targets, view.gbufferDescriptors->GetBindingLayout()));
    view.passes.push_back(std::make_unique<VulkanSelectionMaskPass>(m_nvrhi->Get(), *view.targets, m_frameSetLayout->Get(), m_materialSetLayout->Get()));
    view.passes.push_back(std::make_unique<VulkanSelectionOutlinePass>(m_nvrhi->Get(), *view.targets));
}

void VulkanRenderer::CreateDescriptorResources()
{
    m_view.uniformBuffer = CreateViewUniformBuffer(m_view, m_drawSlotWatermark);
}

std::unique_ptr<VulkanUniformBuffer> VulkanRenderer::CreateViewUniformBuffer(const VulkanSceneView& view, uint32_t drawCapacity) const
{
    auto uniformBuffer = std::make_unique<VulkanUniformBuffer>(
        NativePhysicalDevice(),
        NativeDevice(),
        m_nvrhi->Get(),
        static_cast<uint32_t>(m_swapchain->GetImageCount()),
        m_frameSetLayout->Get(),
        view.shadowPass->GetSampledBinding(),
        m_localShadowPass->GetSampledBinding(),
        BuildEnvironmentBindings(view),
        drawCapacity);
    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
    {
        if (renderSubmesh->drawSlot != RenderSubmesh::kNoDrawSlot)
        {
            uniformBuffer->WriteDrawSlot(renderSubmesh->drawSlot, renderSubmesh->material, renderSubmesh->textureTransforms);
        }
    }
    return uniformBuffer;
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
    // Handed out again only once no frame in flight draws from it: a new draw is written into the
    // per-draw buffers without waiting for the GPU.
    if (slot != RenderSubmesh::kNoDrawSlot)
    {
        Retire([this, slot]()
               {
                   m_freeDrawSlots.push_back(slot);
               });
    }
}

void VulkanRenderer::RunNvrhiCommands(const std::function<void(nvrhi::ICommandList*)>& record)
{
    // Submitted on the frames' queue ahead of the next frame, which therefore sees its results.
    nvrhi::CommandListHandle commandList = m_nvrhi->Get()->createCommandList();
    commandList->open();
    commandList->setEnableAutomaticBarriers(false);
    record(commandList);
    commandList->close();
    m_nvrhi->Get()->executeCommandList(commandList);
}

void VulkanRenderer::Retire(std::function<void()> release)
{
    // After the next frame's submit too: an upload batch submitted before it, in this frame, may use
    // what is retired, and only a later frame's fence covers that batch.
    m_retireQueue.Retire(m_commandContext->LastSubmit() + 1, std::move(release));
}

void VulkanRenderer::DestroyDescriptorResources()
{
    m_view.uniformBuffer.reset();
    // m_textureStore survives this teardown: the next upload reuses every live texture.
}

void VulkanRenderer::RecreateSwapchain()
{
    // Minimized: the old swapchain stays until the window is restored and the next frame rebuilds it.
    const VkExtent2D wanted = WantedSwapchainExtent();
    if (!HasDrawableArea() || wanted.width == 0 || wanted.height == 0)
    {
        return;
    }

    m_nvrhi->Get()->waitForIdle();
    DestroyDescriptorResources();
    DestroySwapchainResources();
    CreateSwapchainResources();
    CreateDescriptorResources();
}

VkExtent2D VulkanRenderer::WantedSwapchainExtent() const
{
    return m_nvrhi->GetSwapchainExtent(GetWindow().GetSDLWindow());
}

VulkanRenderer::SceneExtents VulkanRenderer::ResolveSceneExtents(RenderExtent viewportExtent, const RenderDebugSettings& renderDebug)
{
    SceneExtents extents;
    extents.output = ToVkExtent(viewportExtent);
    extents.render = extents.output;
    // DLSS needs the deferred order's motion vectors; where it cannot run, the engine's TAA resolves
    // at the viewport's size (which the editor already scaled by the render scale).
    if (renderDebug.dlssMode != DlssMode::Off && !renderDebug.forwardOnly && m_dlss->IsAvailable())
    {
        const std::optional<VkExtent2D> render = m_dlss->RenderExtentFor(extents.output, renderDebug.dlssMode);
        if (render.has_value() &&
            m_dlss->EnsureFeature(*render, extents.output, renderDebug.dlssMode, renderDebug.dlssPreset, renderDebug.dlssRayReconstruction))
        {
            extents.render = *render;
            extents.dlss = renderDebug.dlssMode;
            extents.dlssPreset = renderDebug.dlssPreset;
            extents.rayReconstruction = m_dlss->HasRayReconstruction();
            return extents;
        }
    }
    m_dlss->ReleaseFeature();
    return extents;
}

void VulkanRenderer::SyncSceneTargets(RenderExtent viewportExtent, const RenderDebugSettings& renderDebug)
{
    if (!m_swapchain || !m_view.targets || !viewportExtent.IsValid())
    {
        return;
    }

    const SceneExtents extents = ResolveSceneExtents(viewportExtent, renderDebug);
    if (extents.dlss != m_activeDlssMode || extents.dlssPreset != m_activeDlssPreset || extents.rayReconstruction != m_activeDlssRayReconstruction)
    {
        // Another resolve, or DLSS at another quality or with another model: no history carries over.
        LOG_INFO(
            "Temporal resolve: {}",
            extents.dlss == DlssMode::Off ? "TAA" : (extents.rayReconstruction ? "DLSS ray reconstruction" : "DLSS"));
        m_activeDlssMode = extents.dlss;
        m_activeDlssPreset = extents.dlssPreset;
        m_activeDlssRayReconstruction = extents.rayReconstruction;
        m_view.dlssResetPending = true;
        m_view.taaHistory.Reset();
    }
    if (m_view.targets->MatchesExtent(extents.render, extents.output))
    {
        return;
    }
    RebuildViewTargets(m_view, extents.render, extents.output);
    m_view.pathTraceAccumulation.Reset();
    m_view.dlssResetPending = true;
    LOG_INFO(
        "Scene render targets resized to {}x{}, output {}x{}",
        m_view.targets->GetExtent().width,
        m_view.targets->GetExtent().height,
        m_view.targets->GetOutputExtent().width,
        m_view.targets->GetOutputExtent().height);
}

void VulkanRenderer::RebuildViewTargets(VulkanSceneView& view, VkExtent2D render, VkExtent2D output)
{
    // Only the target images and the per-image resources built from them depend on the extent.
    // Both passes' render passes and the targets' sampler survive, and the pipelines use dynamic
    // viewport/scissor state, so neither they nor the uniform buffer have to be rebuilt while the
    // user drags the viewport edge. The images are new, so the tracker goes back to undefined.
    m_nvrhi->Get()->waitForIdle();
    view.targets->Rebuild(render, output, static_cast<uint32_t>(m_swapchain->GetImageCount()));
    view.gbufferDescriptors->OnTargetsRebuilt(*view.targets);
    for (const std::unique_ptr<IScenePass>& pass : view.passes)
    {
        pass->OnTargetsRebuilt(*view.targets);
    }
    // The scatter pass recreated its images at the new extent; set 0 still names the old ones. The
    // path traced layer's are gone until the next path traced frame makes them again.
    if (view.uniformBuffer)
    {
        view.uniformBuffer->SetScatterImages(view.scatterPass->GetLightBinding(), view.scatterPass->GetDepthBinding());
        TextureDescriptorBinding depth;
        TextureDescriptorBinding diffuse;
        TextureDescriptorBinding specular;
        PathTraceLayerBindings(view, depth, diffuse, specular);
        view.uniformBuffer->SetPathTraceLayerImages(depth, diffuse, specular);
    }
    // Likewise the clouds' resolved target.
    if (m_atmosphere->EnsureCloudTarget(*view.atmosphere, view.targets->GetExtent()) && view.uniformBuffer)
    {
        view.uniformBuffer->SetCloudTarget(m_atmosphere->GetCloudTargetBinding(*view.atmosphere));
    }
    view.ResetHistories();
}

void VulkanRenderer::SyncCaptureViews(std::span<const SceneCaptureView> cameras)
{
    if (!m_swapchain || !m_view.targets)
    {
        return;
    }
    if (cameras.size() < m_captureViews.size())
    {
        // The frames in flight may still draw them.
        m_nvrhi->Get()->waitForIdle();
        m_captureViews.resize(cameras.size());
        LOG_INFO("Capture views: {}", cameras.size());
    }
    // Photo Mode's DLSS feature lives as long as its view.
    if (std::none_of(cameras.begin(), cameras.end(), [](const SceneCaptureView& camera) { return camera.photo; }))
    {
        m_dlss->ReleaseFeature(DlssFeatureSlot::Photo);
        m_photoDlssMode = DlssMode::Off;
    }
    m_photoViewIndex.reset();
    for (size_t index = 0; index < cameras.size(); ++index)
    {
        const SceneCaptureView& capture = cameras[index];
        const VkExtent2D output = ToVkExtent(capture.extent);
        bool dlss = false;
        const VkExtent2D render = capture.photo ? EnsurePhotoDlss(capture, output, dlss) : output;
        try
        {
            if (index == m_captureViews.size())
            {
                m_captureViews.push_back(CreateCaptureView(render, output));
                LOG_INFO(
                    "Capture view {} made at {}x{}, output {}x{} ({} MB of targets)",
                    index,
                    render.width,
                    render.height,
                    output.width,
                    output.height,
                    m_captureViews.back()->targets->GetAllocatedBytes() >> 20);
            }
            else if (!m_captureViews[index]->targets->MatchesExtent(render, output))
            {
                RebuildViewTargets(*m_captureViews[index], render, output);
                LOG_INFO("Capture view {} resized to {}x{}, output {}x{}", index, render.width, render.height, output.width, output.height);
            }
        }
        catch (const std::exception& error)
        {
            // A photo can ask for more than the GPU has; the views made so far still draw, and the
            // photo reports why it has none.
            if (!capture.photo)
            {
                throw;
            }
            m_nvrhi->Get()->waitForIdle();
            m_captureViews.resize(index);
            m_dlss->ReleaseFeature(DlssFeatureSlot::Photo);
            m_photoDlssMode = DlssMode::Off;
            m_photoViewError = fmt::format("its {}x{} view could not be made: {}", output.width, output.height, error.what());
            LOG_ERROR("Photo: {}", m_photoViewError);
            return;
        }
        VulkanSceneView& view = *m_captureViews[index];
        view.dlssSlot = capture.photo ? DlssFeatureSlot::Photo : DlssFeatureSlot::Viewport;
        view.dlssActive = dlss;
        if (capture.photo)
        {
            m_photoViewIndex = index;
            m_photoViewError.clear();
            // Another resolve, or DLSS at another quality, model or denoiser: no history carries over.
            const DlssMode mode = dlss ? capture.dlssMode : DlssMode::Off;
            const bool rayReconstruction = dlss && m_dlss->HasRayReconstruction(DlssFeatureSlot::Photo);
            if (mode != m_photoDlssMode || capture.dlssPreset != m_photoDlssPreset || rayReconstruction != m_photoDlssRayReconstruction)
            {
                LOG_INFO("Photo: resolves with {}", mode == DlssMode::Off ? "TAA" : (rayReconstruction ? "DLSS ray reconstruction" : "DLSS"));
                m_photoDlssMode = mode;
                m_photoDlssPreset = capture.dlssPreset;
                m_photoDlssRayReconstruction = rayReconstruction;
                view.ResetHistories();
            }
        }
    }
}

VkExtent2D VulkanRenderer::EnsurePhotoDlss(const SceneCaptureView& capture, VkExtent2D output, bool& dlss)
{
    dlss = false;
    if (capture.dlssMode != DlssMode::Off && m_dlss->IsAvailable())
    {
        const std::optional<VkExtent2D> render = m_dlss->RenderExtentFor(output, capture.dlssMode, DlssFeatureSlot::Photo);
        if (render.has_value() &&
            m_dlss->EnsureFeature(*render, output, capture.dlssMode, capture.dlssPreset, capture.dlssRayReconstruction, DlssFeatureSlot::Photo))
        {
            dlss = true;
            return *render;
        }
        // The feature would not be made (NGX's log says why): the TAA resolves the photo instead.
    }
    m_dlss->ReleaseFeature(DlssFeatureSlot::Photo);
    return output;
}

std::unique_ptr<VulkanSceneView> VulkanRenderer::CreateCaptureView(VkExtent2D render, VkExtent2D output)
{
    // Its targets in the viewport's LDR format, so the views' pictures can share a canvas and the
    // material pipelines (made against the viewport's passes) draw into them.
    auto view = std::make_unique<VulkanSceneView>();
    view->targets = std::make_unique<SceneRenderTargets>(
        NativePhysicalDevice(),
        NativeDevice(),
        m_nvrhi->Get(),
        m_view.targets->GetFormat(RenderTargetId::SceneLdr),
        render,
        output,
        static_cast<uint32_t>(m_swapchain->GetImageCount()));
    view->shadowPass = std::make_unique<VulkanShadowPass>(
        m_nvrhi->Get(), m_frameSetLayout->Get(), m_materialSetLayout->Get(), kShadowMapResolution);
    view->atmosphere = m_atmosphere->CreateView();
    m_atmosphere->EnsureCloudTarget(*view->atmosphere, render);
    CreateScenePasses(*view);
    view->uniformBuffer = CreateViewUniformBuffer(*view, m_view.uniformBuffer->GetDrawCapacity());
    // The environment map the viewport's sets name, which the constructor's bindings already do.
    return view;
}

void VulkanRenderer::UploadSceneResources(const RenderFramePacket& frame)
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

    // Batch every texture and submesh-buffer upload below into a handful of submits instead of
    // one per resource: VulkanTexture/VulkanBuffer used to each own their
    // upload (command pool, submit, vkQueueWaitIdle), which serializes hundreds of GPU
    // round-trips in a row for models with many submeshes/textures (e.g. Sponza: 405 submeshes,
    // up to ~170 unique textures). Flushing periodically bounds how much staging memory is held
    // at once while still cutting the number of GPU stalls by roughly two orders of magnitude. The
    // batch stages out of its own few chunks, and flushes once this much is staged: a count of
    // resources flushed a streamed cell's thousand small buffers sixteen times, each a queue wait.
    // The batches are submitted without a wait (VulkanUploadBatch::SubmitWithoutWait): the frames that
    // draw the new content come after them on the queue. A failed upload waits for them all before it
    // drops what they uploaded into.
    constexpr VkDeviceSize kStagedBytesPerUploadFlush = VkDeviceSize{64} << 20;
    const auto makeUploadBatch = [this]()
    {
        auto batch = std::make_unique<VulkanUploadBatch>(m_nvrhi->Get(), m_uploadPool.get());
        return batch;
    };
    std::unique_ptr<VulkanUploadBatch> uploadBatch = makeUploadBatch();
    const auto submitUploadBatch = [&]()
    {
        if (!uploadBatch->IsEmpty())
        {
            uploadBatch->SubmitWithoutWait();
            m_uploadBatches.push_back(std::move(uploadBatch));
            uploadBatch = makeUploadBatch();
        }
    };
    auto flushUploadBatchIfNeeded = [&]()
    {
        if (uploadBatch->StagedBytes() >= kStagedBytesPerUploadFlush)
        {
            submitUploadBatch();
        }
    };

    // Material texture files are uploaded block-compressed whenever the device allows it.
    const bool compressTextures = m_nvrhi->SupportsBlockCompression();

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
        auto texture = std::make_unique<VulkanTexture>(NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(), data, *uploadBatch, fmt);
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
                PrepareTexture(texturePath, usage, compressTextures, DefaultTextureCacheDirectory()),
                usage,
                *uploadBatch);
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
        // The batches already submitted may still be copying into what goes here.
        m_uploadBatches.clear();
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
    // A large change commits over several frames: new submeshes are made until the budget is spent (at
    // least kMinNewSubmeshesPerCommit of them), the rest wait for the next frames' commits, which keep
    // what this one made. Making a submesh and then its descriptors and ray material cost ~10 us each in
    // Release, and a map's first load committed 15,000 in one frame of 120 ms.
    constexpr size_t kMinNewSubmeshesPerCommit = 256;
    constexpr double kCommitBuildBudgetMs = 5.0;
    size_t deferredSubmeshCount = 0;
    // The time spent making new submeshes (not walking the kept ones), what the budget measures.
    double makingMs = 0.0;
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
        // holds is the same data, so its buffers carry over (m_meshBuffers).
        if (m_meshBuffers.size() > 2 * m_renderSubmeshes.size() + 1024)
        {
            std::erase_if(m_meshBuffers, [](const auto& entry)
                          {
                              return entry.second.buffer.expired() || entry.second.mesh.expired();
                          });
        }
        // An entity's submeshes come one after another: the ordinal counts along a run, and the map is
        // touched only where the entity changes.
        entt::entity runEntity = entt::null;
        uint32_t runOrdinal = 0;
        for (const std::shared_ptr<const CpuRenderSubmesh>& entry : *frame.renderSubmeshes)
        {
            const CpuRenderSubmesh& cpuRenderSubmesh = *entry;
            if (cpuRenderSubmesh.entity != runEntity)
            {
                if (runEntity != entt::null)
                {
                    nextSubmeshOrdinal[runEntity] = runOrdinal;
                }
                runEntity = cpuRenderSubmesh.entity;
                const auto counted = nextSubmeshOrdinal.find(runEntity);
                runOrdinal = counted != nextSubmeshOrdinal.end() ? counted->second : 0u;
            }
            const uint32_t ordinal = runOrdinal++;

            // Kept: a submesh the GPU already has, with its textures, material set and draw slot.
            if (const auto live = m_liveSubmeshes.find(cpuRenderSubmesh.revision); live != m_liveSubmeshes.end())
            {
                newRenderSubmeshes.push_back(live->second);
                ++keptSubmeshCount;
                continue;
            }
            // Over the budget: left for the next commit.
            if (deferredSubmeshCount > 0 || (madeSubmeshes.size() >= kMinNewSubmeshesPerCommit && makingMs > kCommitBuildBudgetMs))
            {
                ++deferredSubmeshCount;
                continue;
            }
            const auto makingStart = std::chrono::steady_clock::now();

            auto renderSubmesh = std::make_shared<RenderSubmesh>();
            renderSubmesh->entity = cpuRenderSubmesh.entity;
            renderSubmesh->revision = cpuRenderSubmesh.revision;
            renderSubmesh->motionKey = MotionKey{static_cast<uint32_t>(entt::to_integral(cpuRenderSubmesh.entity)), ordinal};
            renderSubmesh->mesh = cpuRenderSubmesh.mesh;
            // A posed mesh's buffers are the skinning pass's output for this submesh alone: another
            // entity with the same model poses its own copy.
            std::shared_ptr<VulkanBuffer> liveBuffer;
            if (const auto prepared = m_preparedBuffers.find(cpuRenderSubmesh.mesh.get());
                prepared != m_preparedBuffers.end() && prepared->second.buffer && prepared->second.mesh == cpuRenderSubmesh.mesh)
            {
                liveBuffer = prepared->second.buffer;
                m_meshBuffers[cpuRenderSubmesh.mesh.get()] = MeshBuffers{cpuRenderSubmesh.mesh, liveBuffer};
            }
            else if (const auto live = m_meshBuffers.find(cpuRenderSubmesh.mesh.get());
                live != m_meshBuffers.end() && !cpuRenderSubmesh.mesh->IsPosed() && live->second.mesh.lock() == cpuRenderSubmesh.mesh)
            {
                liveBuffer = live->second.buffer.lock();
            }
            if (liveBuffer)
            {
                renderSubmesh->buffer = std::move(liveBuffer);
            }
            else
            {
                // Addressable for the ray scene's hit shading when rays run on the hardware.
                renderSubmesh->buffer = std::make_shared<VulkanBuffer>(
                    NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(),
                    *cpuRenderSubmesh.mesh, *uploadBatch, m_nvrhi->SupportsRayQuery());
                if (!cpuRenderSubmesh.mesh->IsPosed())
                {
                    m_meshBuffers[cpuRenderSubmesh.mesh.get()] = MeshBuffers{cpuRenderSubmesh.mesh, renderSubmesh->buffer};
                }
                ++newBufferCount;
                flushUploadBatchIfNeeded();
            }
            renderSubmesh->material = cpuRenderSubmesh.material;
            renderSubmesh->textureTransforms = BuildGpuTextureTransforms(cpuRenderSubmesh.textureTransforms);
            renderSubmesh->doubleSided = cpuRenderSubmesh.doubleSided;
            renderSubmesh->alphaMode = cpuRenderSubmesh.alphaMode;
            renderSubmesh->decal = cpuRenderSubmesh.decal;
            renderSubmesh->toon = cpuRenderSubmesh.toon;
            renderSubmesh->skinned = cpuRenderSubmesh.skinned && renderSubmesh->buffer->IsSkinned();
            renderSubmesh->paletteOffset = cpuRenderSubmesh.paletteOffset;
            renderSubmesh->jointCount = cpuRenderSubmesh.jointCount;
            renderSubmesh->toonHeadJoint = cpuRenderSubmesh.toonHeadJoint;
            renderSubmesh->tyre = cpuRenderSubmesh.mesh->deformable && renderSubmesh->buffer->IsPosed();
            if (renderSubmesh->skinned || renderSubmesh->tyre)
            {
                renderSubmesh->skinningSet = m_skinningPass->Acquire(*renderSubmesh->buffer);
            }
            renderSubmesh->localBoundsCenter = cpuRenderSubmesh.localBoundsCenter;
            renderSubmesh->localBoundsRadius = cpuRenderSubmesh.localBoundsRadius;
            renderSubmesh->castShadows = cpuRenderSubmesh.castShadows;
            renderSubmesh->drawDistance = cpuRenderSubmesh.drawDistance;
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
            makingMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - makingStart).count();
        }

        // The new submeshes' material sets: kept where another submesh already has the same textures.
        const std::vector<MaterialTextureBinding> bindings = BuildMaterialTextureBindings(textureViews, madeSlots, *m_samplerCache);
        for (size_t index = 0; index < madeSubmeshes.size(); ++index)
        {
            madeSubmeshes[index]->materialSet = m_materialSets->Acquire(bindings[index]);
            PackMaterialSamplerIndices(madeSlots[index], madeSubmeshes[index]->material.samplerIndices);
            madeSubmeshes[index]->rayBaseColor = bindings[index].baseColor;
            madeSubmeshes[index]->rayEmissive = bindings[index].emissive;
            madeSubmeshes[index]->rayMetallic = bindings[index].metallic;
            madeSubmeshes[index]->rayRoughness = bindings[index].roughness;
            // Only a map of its own: the ray material says the flat one is not worth a fetch.
            madeSubmeshes[index]->rayNormal = madeSlots[index].normal != defaultNormalIndex ? bindings[index].normal : TextureDescriptorBinding{};
        }
        if (!uploadBatch->IsEmpty())
        {
            uploadBatch->SubmitWithoutWait();
            m_uploadBatches.push_back(std::move(uploadBatch));
        }
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
    m_unreferencedTextureKeys.insert(m_unreferencedTextureKeys.end(), addedTextureKeys.begin(), addedTextureKeys.end());
    const auto applyStart = std::chrono::steady_clock::now();
    // A change too large for one commit is a map loading, which keeps adding cells: room for four times
    // what it holds so far (up to 65,536 draws, or a quarter more than it holds), made while few draws
    // are live (a growth writes every live draw again, and allocating for 126,000 took 50 ms).
    const size_t changeSize = submeshCount + deferredSubmeshCount;
    m_drawSlotReserve =
        static_cast<uint32_t>(deferredSubmeshCount > 0 ? std::max(std::min<size_t>(changeSize * 4, 65536), changeSize + changeSize / 4) : submeshCount);
    try
    {
        ApplyRenderContent(std::move(newRenderSubmeshes), keptSubmeshCount, frame);
    }
    catch (...)
    {
        dropAdded();
        throw;
    }
    // The requests of what is on the GPU now are done; the rest of a deferred change commits next.
    std::erase_if(m_requestedRevisions, [this](uint64_t revision)
                  {
                      return m_liveSubmeshes.count(revision) != 0;
                  });
    if (deferredSubmeshCount > 0)
    {
        m_sceneUploadPending = true;
    }
    else if (m_requestedRevisions.size() > 4096)
    {
        // Requests of submeshes that left before they committed: asked for again if they come back.
        m_requestedRevisions.clear();
    }
    LOG_INFO(
        "Uploaded {} submeshes ({} kept, {} new buffers, {} textures stored{}) in {:.0f} ms, then {:.0f} ms for descriptors and the ray scene",
        submeshCount,
        keptSubmeshCount,
        newBufferCount,
        m_textureStore.size(),
        deferredSubmeshCount > 0 ? ", " + std::to_string(deferredSubmeshCount) + " left for the next frames" : std::string(),
        uploadMs,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count());
}

void VulkanRenderer::DropUnreferencedTextures()
{
    // Destroyed once the frames in flight, which may still sample them, have finished. Only the textures
    // that lost their last reference or were just stored can have none.
    auto dropped = std::make_shared<std::vector<std::unique_ptr<VulkanTexture>>>();
    for (const std::string& key : m_unreferencedTextureKeys)
    {
        if (auto entry = m_textureStore.find(key); entry != m_textureStore.end() && entry->second.references == 0 && !entry->second.permanent)
        {
            dropped->push_back(std::move(entry->second.texture));
            m_textureStore.erase(entry);
        }
    }
    m_unreferencedTextureKeys.clear();
    if (!dropped->empty())
    {
        Retire([dropped]()
               {
                   dropped->clear();
               });
    }
}

void VulkanRenderer::UploadSceneResourcesOrKeepPrevious(const RenderFramePacket& frame)
{
    try
    {
        UploadSceneResources(frame);
    }
    catch (const std::exception& error)
    {
        if (!IsOutOfMemoryError(error))
        {
            throw;
        }
        LOG_ERROR("Keeping the previous scene content, the upload ran out of GPU memory: {}", error.what());
        m_outOfMemoryChange = true;
        AbandonPendingTextures();
        DropSubmeshesOfRemovedEntities(frame);
        return;
    }

    // The rest of a large change commits over the next frames, with the textures and buffers prepared
    // for it.
    if (m_sceneUploadPending)
    {
        return;
    }

    // Prepared buffers the change did not use (a submesh dropped again before it committed) go once
    // their uploads have run; the used ones live on in their submeshes.
    ReleasePreparedBuffers();

    // Staged textures the scene no longer needed are released with the change they were for, once
    // their uploads have run (rarely any: a change stages only what it asks for).
    if (!m_stagedTextures.empty())
    {
        auto unused = std::make_shared<std::unordered_map<std::string, std::unique_ptr<VulkanTexture>>>(std::move(m_stagedTextures));
        Retire([unused]()
               {
                   unused->clear();
               });
    }
    m_stagedTextures.clear();
    m_failedTextureKeys.clear();
    m_texturesRequested = 0;
    m_textureUploadStats = TextureUploadStats{};

    // The screen matches the scene again, so a report of it not matching is now stale.
    m_outOfMemoryChange = false;
}

void VulkanRenderer::RequestSceneUpload(const RenderFramePacket& frame)
{
    // The previous content stays on screen until the change commits, so it must stop drawing any
    // entity the change deleted right away.
    DropSubmeshesOfRemovedEntities(frame);

    for (const std::shared_ptr<const CpuRenderSubmesh>& entry : *frame.renderSubmeshes)
    {
        const CpuRenderSubmesh& submesh = *entry;
        if (m_liveSubmeshes.count(submesh.revision) != 0 || !m_requestedRevisions.insert(submesh.revision).second)
        {
            continue;
        }
        // A new submesh of a mesh with no GPU buffers yet: made before the commit. A posed mesh's are
        // its own, made with it.
        if (submesh.mesh && !submesh.mesh->IsPosed() && m_preparedBuffers.count(submesh.mesh.get()) == 0)
        {
            const auto live = m_meshBuffers.find(submesh.mesh.get());
            if (live == m_meshBuffers.end() || live->second.buffer.expired() || live->second.mesh.lock() != submesh.mesh)
            {
                m_preparedBuffers.emplace(submesh.mesh.get(), PreparedBuffers{submesh.mesh, nullptr});
                m_meshesToUpload.push_back(submesh.mesh);
            }
        }
        // A submesh the GPU already draws has every texture it names.
        if (!submesh.hasTexCoords)
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

void VulkanRenderer::PumpSceneUpload(const RenderFramePacket& frame)
{
    // A few per frame, so the frame loop keeps its pace while a large scene streams in: the new meshes'
    // buffers (~10 us each) and the prepared textures (a memcpy into the batch's staging and a pooled
    // image each, ~0.1 ms; a cell brings hundreds), together within a few milliseconds, or a share of the
    // frame where frames are long (a Debug build, where both cost several times as much and a fixed
    // budget held cells back for seconds). Four textures a frame held a cell back for seconds, and 64
    // with a cell's buffers took 12 ms of one frame. While a backlog as large as a scene's first load
    // waits there is no time limit: the commit waits for all of a change, and a load staged slowly while
    // cells kept coming never caught up with them.
    constexpr size_t kStagedTexturesPerFrame = 64;
    // At least this many whatever they cost (in a Release build about as long as the budget).
    constexpr size_t kMinStagedTexturesPerFrame = 32;
    constexpr size_t kStagedTexturesPerGroup = 8;
    double averageFrameMs = 0.0;
    for (const double ms : m_cpuFrameMs)
    {
        averageFrameMs += ms / static_cast<double>(m_cpuFrameMs.size());
    }
    const double meshUploadBudgetMs = std::max(2.0, averageFrameMs * 0.15);
    const double stagingBudgetMs = std::max(4.0, averageFrameMs * 0.3);
    constexpr size_t kTextureBacklog = 1024;
    constexpr size_t kMeshUploadBacklog = 2048;
    std::erase_if(m_uploadBatches, [](const std::unique_ptr<VulkanUploadBatch>& batch)
                  {
                      return batch->IsComplete();
                  });

    // Results of a change that was abandoned are dropped; a later change prepares what it needs
    // again, from the compressed texture cache.
    bool staged = false;
    if (!m_sceneUploadPending)
    {
        m_texturePreparation->TakeCompleted(kStagedTexturesPerFrame);
    }
    else
    {
        try
        {
            const auto stagingStart = std::chrono::steady_clock::now();
            const auto elapsedMs = [&stagingStart]()
            {
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stagingStart).count();
            };
            std::unique_ptr<VulkanUploadBatch> uploadBatch;
            const auto batch = [&]() -> VulkanUploadBatch&
            {
                if (!uploadBatch)
                {
                    uploadBatch = std::make_unique<VulkanUploadBatch>(m_nvrhi->Get(), m_uploadPool.get());
                }
                return *uploadBatch;
            };
            const bool unlimitedMeshes = m_meshesToUpload.size() > kMeshUploadBacklog;
            while (!m_meshesToUpload.empty() && (unlimitedMeshes || elapsedMs() < meshUploadBudgetMs))
            {
                const std::shared_ptr<const MeshData> mesh = std::move(m_meshesToUpload.front());
                m_meshesToUpload.pop_front();
                // Addressable for the ray scene's hit shading when rays run on the hardware.
                m_preparedBuffers[mesh.get()] = PreparedBuffers{
                    mesh,
                    std::make_shared<VulkanBuffer>(NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(), *mesh, batch(), m_nvrhi->SupportsRayQuery())};
            }
            // A backlog as large as a map's first load stages for longer, though never without a limit:
            // 64 large textures took over 50 ms of one frame.
            const bool backlog = m_texturePreparation->PendingCount() > kTextureBacklog;
            const double textureBudgetMs = backlog ? std::max(stagingBudgetMs, 10.0) : stagingBudgetMs;
            size_t stagedTextures = 0;
            // The floor yields to a hard limit too: textures that each took a new memory block or a large
            // copy made the floor alone a 50 ms frame.
            constexpr double kStagingHardLimitMs = 15.0;
            while (stagedTextures < kStagedTexturesPerFrame &&
                   (stagedTextures == 0 || ((stagedTextures < kMinStagedTexturesPerFrame || elapsedMs() < textureBudgetMs) && elapsedMs() < kStagingHardLimitMs)))
            {
                std::vector<TexturePreparationResult> completed = m_texturePreparation->TakeCompleted(kStagedTexturesPerGroup);
                if (completed.empty())
                {
                    break;
                }
                staged = true;
                for (TexturePreparationResult& result : completed)
                {
                    ++stagedTextures;
                    if (!result.texture)
                    {
                        LOG_ERROR("Failed to load model texture '{}': {}", result.key, result.error);
                        m_failedTextureKeys.insert(result.key);
                        continue;
                    }
                    m_stagedTextures[result.key] = UploadPreparedTexture(*result.texture, result.usage, batch());
                }
            }
            if (uploadBatch)
            {
                // The frames after this one draw with these only once the change commits, and the batch's
                // barrier orders its copies before them: nothing here waits for the GPU.
                uploadBatch->SubmitWithoutWait();
                m_uploadBatches.push_back(std::move(uploadBatch));
            }
        }
        catch (const std::exception& error)
        {
            if (!IsOutOfMemoryError(error))
            {
                throw;
            }
            LOG_ERROR("Keeping the previous scene content, staging a texture ran out of GPU memory: {}", error.what());
            m_outOfMemoryChange = true;
            AbandonPendingTextures();
        }
    }

    m_cpuStages.Mark("StageTextures");
    // Not in the frame that staged the change's last textures: the two together made one long frame.
    // Meshes still waiting for their buffers do not hold the commit back, which makes the rest itself:
    // the buffers are made ahead only while the textures are being prepared (a Debug build makes them
    // so slowly that waiting for all of them held cells back for seconds).
    if (m_sceneUploadPending && m_texturePreparation->IsIdle() && !staged)
    {
        m_sceneUploadPending = false;
        UploadSceneResourcesOrKeepPrevious(frame);
        m_cpuStages.Mark("CommitContent");
    }

    if (m_sceneUploadPending)
    {
        const size_t pending = m_texturePreparation->PendingCount();
        const size_t done = m_texturesRequested > pending ? m_texturesRequested - pending : 0;
        m_sceneUploadStatus =
            "Preparing textures: " + std::to_string(done) + " of " + std::to_string(m_texturesRequested);
        if (!m_meshesToUpload.empty())
        {
            m_sceneUploadStatus += ", " + std::to_string(m_meshesToUpload.size()) + " meshes to upload";
        }
    }
    else
    {
        m_sceneUploadStatus.clear();
    }
}

void VulkanRenderer::ReleasePreparedBuffers()
{
    m_meshesToUpload.clear();
    if (m_preparedBuffers.empty())
    {
        return;
    }
    auto prepared = std::make_shared<std::unordered_map<const MeshData*, PreparedBuffers>>(std::move(m_preparedBuffers));
    m_preparedBuffers.clear();
    Retire([prepared]()
           {
               prepared->clear();
           });
}

void VulkanRenderer::AbandonPendingTextures()
{
    // Staged textures are referenced by no descriptor set; they go once the batches uploading them have
    // run.
    m_sceneUploadPending = false;
    m_requestedRevisions.clear();
    m_uploadBatches.clear();
    ReleasePreparedBuffers();
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
            NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(),
            *prepared.halfFloat, uploadBatch);
    }
    if (!prepared.compressed)
    {
        ++stats.uncompressed;
        return std::make_unique<VulkanTexture>(
            NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(),
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
        NativePhysicalDevice(), NativeDevice(), m_nvrhi->Get(),
        *prepared.compressed, uploadBatch);
}

void VulkanRenderer::DropSubmeshesOfRemovedEntities(const RenderFramePacket& frame)
{
    const auto isRemoved = [&frame](const std::shared_ptr<const RenderSubmesh>& renderSubmesh)
    {
        return !frame.transforms.Contains(renderSubmesh->entity);
    };
    if (std::none_of(m_renderSubmeshes.begin(), m_renderSubmeshes.end(), isRemoved))
    {
        return;
    }

    // The frames in flight may still draw from their buffers: the submeshes are retired, not
    // destroyed. What remains is a subset of the list the uniform buffer was sized for, so its motion
    // slots still cover it; material binding indices and motion keys are per submesh and stay valid.
    auto removed = std::make_shared<std::vector<std::shared_ptr<const RenderSubmesh>>>();
    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
    {
        if (!isRemoved(renderSubmesh))
        {
            continue;
        }
        removed->push_back(renderSubmesh);
        if (renderSubmesh->drawSlot != RenderSubmesh::kNoDrawSlot)
        {
            ReleaseDrawSlot(renderSubmesh->drawSlot);
            renderSubmesh->drawSlot = RenderSubmesh::kNoDrawSlot;
            for (const std::string& key : renderSubmesh->textureKeys)
            {
                if (auto entry = m_textureStore.find(key); entry != m_textureStore.end() && entry->second.references > 0 &&
                                                           --entry->second.references == 0)
                {
                    m_unreferencedTextureKeys.push_back(key);
                }
            }
            m_materialSets->Release(renderSubmesh->materialSet);
            renderSubmesh->skinningSet = nullptr;
        }
    }
    std::erase_if(m_renderSubmeshes, isRemoved);
    std::erase_if(m_liveSubmeshes, [&isRemoved](const auto& entry)
                  {
                      return isRemoved(entry.second);
                  });
    Retire([removed]()
           {
               removed->clear();
           });
}

void VulkanRenderer::ApplyRenderContent(
    std::vector<std::shared_ptr<const RenderSubmesh>> newRenderSubmeshes,
    size_t keptSubmeshCount,
    const RenderFramePacket& frame)
{
    // Up to the ray scene's content everything may throw and leaves the old content drawable;
    // afterwards nothing does. A change costs what it adds and drops: kept draws keep their slots,
    // textures, material sets and ray materials.
    if (m_swapchain && !m_backBuffers.empty() && !m_view.passes.empty())
    {
        // Draws new to this content, and the old content's draws it drops (every kept one is in both).
        const uint64_t commit = ++m_commitSerial;
        std::vector<const RenderSubmesh*> placed;
        for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : newRenderSubmeshes)
        {
            if (renderSubmesh->drawSlot == RenderSubmesh::kNoDrawSlot)
            {
                placed.push_back(renderSubmesh.get());
            }
            else
            {
                renderSubmesh->commitSerial = commit;
            }
        }
        std::vector<const RenderSubmesh*> dropped;
        std::vector<uint32_t> releasedSlots;
        for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : m_renderSubmeshes)
        {
            if (renderSubmesh->drawSlot != RenderSubmesh::kNoDrawSlot && renderSubmesh->commitSerial != commit)
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
            renderSubmesh->commitSerial = commit;
        }
        // New draws go into slots no frame in flight reads, and what the dropped ones held is retired, so
        // nothing here waits for the GPU; unless the per-draw buffers (and with them the ray scene's
        // slots) grow, which replaces what the frames in flight read.
        const bool grows = !m_view.uniformBuffer || m_view.uniformBuffer->GetDrawCapacity() < m_drawSlotWatermark;
        if (grows || m_rayScene->ContentChangeWaitsForFrames())
        {
            m_commandContext->WaitForAllFrames();
        }
        try
        {
            if (grows)
            {
                // More draws than the per-draw buffers hold: larger ones, with every draw of the old
                // content and the new written in, so either can be drawn from them.
                // Every view's, as each draws from its own.
                const uint32_t capacity = std::max(
                    {m_drawSlotWatermark, m_drawSlotReserve, m_view.uniformBuffer ? m_view.uniformBuffer->GetDrawCapacity() * 2 : 0u, 256u});
                const auto grow = [&](const VulkanSceneView& view)
                {
                    // The old content's draws, then the new ones.
                    std::unique_ptr<VulkanUniformBuffer> grown = CreateViewUniformBuffer(view, capacity);
                    for (const std::shared_ptr<const RenderSubmesh>& renderSubmesh : newRenderSubmeshes)
                    {
                        if (renderSubmesh->drawSlot != RenderSubmesh::kNoDrawSlot)
                        {
                            grown->WriteDrawSlot(renderSubmesh->drawSlot, renderSubmesh->material, renderSubmesh->textureTransforms);
                        }
                    }
                    return grown;
                };
                std::unique_ptr<VulkanUniformBuffer> grownViewport = grow(m_view);
                std::vector<std::unique_ptr<VulkanUniformBuffer>> grownCaptures;
                for (const std::unique_ptr<VulkanSceneView>& view : m_captureViews)
                {
                    grownCaptures.push_back(grow(*view));
                }
                m_view.uniformBuffer = std::move(grownViewport);
                for (size_t index = 0; index < m_captureViews.size(); ++index)
                {
                    m_captureViews[index]->uniformBuffer = std::move(grownCaptures[index]);
                }
            }
            else
            {
                // Free slots, which no draw of the old content reads.
                for (const RenderSubmesh* renderSubmesh : placed)
                {
                    m_view.uniformBuffer->WriteDrawSlot(renderSubmesh->drawSlot, renderSubmesh->material, renderSubmesh->textureTransforms);
                    for (const std::unique_ptr<VulkanSceneView>& view : m_captureViews)
                    {
                        view->uniformBuffer->WriteDrawSlot(renderSubmesh->drawSlot, renderSubmesh->material, renderSubmesh->textureTransforms);
                    }
                }
            }

            // The ray scene: every draw's mesh, slot and where it is now (for the worker's top level),
            // tens of thousands on a map, made in parallel; and the ray materials of the slots that
            // change hands.
            std::vector<RaySceneSubmesh> raySubmeshes(newRenderSubmeshes.size());
            std::vector<glm::mat4> rayModels(newRenderSubmeshes.size());
            TaskSystem::ParallelFor(
                static_cast<uint32_t>(newRenderSubmeshes.size()),
                1024,
                [&](uint32_t begin, uint32_t end)
                {
                    for (uint32_t index = begin; index < end; ++index)
                    {
                        const RenderSubmesh& renderSubmesh = *newRenderSubmeshes[index];
                        // Rays see a track's nearest level of detail at every distance: a far one would
                        // stand in the same place. Blend surfaces only the path tracer's rays meet.
                        const bool farLevel = renderSubmesh.drawDistance.min > 0.0f;
                        const bool blend = renderSubmesh.alphaMode == MaterialAlphaMode::Blend;
                        const uint32_t flags = (farLevel ? kRayInstanceSkip : blend ? kRayInstanceBlend : 0u) |
                                               (renderSubmesh.castShadows ? 0u : kRayInstanceNoShadow);
                        raySubmeshes[index] = RaySceneSubmesh{renderSubmesh.mesh, renderSubmesh.buffer, flags, renderSubmesh.drawSlot};
                        rayModels[index] = frame.transforms.GetSubmeshModelMatrix(renderSubmesh.entity, renderSubmesh.motionKey.submeshOrdinal);
                    }
                },
                TaskPriority::High);
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
                    renderSubmesh->rayEmissive,
                    renderSubmesh->rayMetallic,
                    renderSubmesh->rayRoughness,
                    renderSubmesh->rayNormal});
            }
            m_rayScene->SetContent(
                std::move(raySubmeshes), std::move(rayModels), m_view.uniformBuffer->GetDrawCapacity(), placedMaterials, releasedSlots);
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
                if (auto entry = m_textureStore.find(key); entry != m_textureStore.end() && entry->second.references > 0 &&
                                                           --entry->second.references == 0)
                {
                    m_unreferencedTextureKeys.push_back(key);
                }
            }
            m_materialSets->Release(renderSubmesh->materialSet);
            renderSubmesh->skinningSet = nullptr;
            m_liveSubmeshes.erase(renderSubmesh->revision);
        }
        for (const RenderSubmesh* renderSubmesh : placed)
        {
            m_liveSubmeshes.emplace(renderSubmesh->revision, renderSubmesh->shared_from_this());
        }
        // The material sets no draw names any more go, before the textures they name are destroyed.
        m_materialSets->FreeUnreferenced([this](std::function<void()> release)
                                         {
                                             Retire(std::move(release));
                                         });
    }

    // The textures no draw names any more, after the material sets that named them (both retired until
    // the frames that sampled them have finished).
    DropUnreferencedTextures();
    const bool newWorld = keptSubmeshCount * 2 < newRenderSubmeshes.size();
    // The old list holds the dropped submeshes, whose buffers the frames in flight may still draw from.
    auto previous = std::make_shared<std::vector<std::shared_ptr<const RenderSubmesh>>>(std::move(m_renderSubmeshes));
    m_renderSubmeshes = std::move(newRenderSubmeshes);
    Retire([previous]()
           {
               previous->clear();
           });
    // New content may be a different world (a scene that finished loading in the background while
    // the startup scene was on screen). The long-term exposure restarts its warm-up, so it catches
    // up at the short-term rates instead of keeping the old world's light for minutes; the view
    // itself does not snap. A streamed world's cells coming and going is the same world.
    if (newWorld)
    {
        m_view.autoExposureState.meteredSeconds = 0.0f;
    }
}

std::vector<VulkanDrawItem> VulkanRenderer::BuildDrawItems(
    uint32_t imageIndex,
    std::span<const glm::mat4> models,
    const glm::mat4& viewProjection,
    const glm::mat4& view) const
{
    const ViewFrustum frustum(viewProjection);
    // Culled and keyed in chunks on the task system; the chunks joined in order are the list one loop
    // over every submesh makes.
    const uint32_t chunkCount = (static_cast<uint32_t>(m_renderSubmeshes.size()) + kSubmeshesPerTask - 1) / kSubmeshesPerTask;
    std::vector<std::vector<VulkanDrawItem>> chunkItems(chunkCount);
    std::vector<std::vector<MaterialDrawSortKey>> chunkKeys(chunkCount);
    TaskSystem::ParallelFor(chunkCount, 1, [&](uint32_t firstChunk, uint32_t endChunk)
                            {
                                for (uint32_t chunk = firstChunk; chunk < endChunk; ++chunk)
                                {
                                    const size_t begin = static_cast<size_t>(chunk) * kSubmeshesPerTask;
                                    const size_t end = std::min(begin + kSubmeshesPerTask, m_renderSubmeshes.size());
                                    for (size_t submeshIndex = begin; submeshIndex < end; ++submeshIndex)
                                    {
                                        AppendDrawItem(
                                            *m_renderSubmeshes[submeshIndex], models[submeshIndex], frustum, view, chunkItems[chunk], chunkKeys[chunk]);
                                    }
                                }
                            });
    std::vector<VulkanDrawItem> unsorted;
    std::vector<MaterialDrawSortKey> sortKeys;
    unsorted.reserve(m_renderSubmeshes.size());
    sortKeys.reserve(m_renderSubmeshes.size());
    for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
    {
        unsorted.insert(unsorted.end(), std::make_move_iterator(chunkItems[chunk].begin()), std::make_move_iterator(chunkItems[chunk].end()));
        sortKeys.insert(sortKeys.end(), chunkKeys[chunk].begin(), chunkKeys[chunk].end());
    }

    std::vector<VulkanDrawItem> ordered;
    ordered.reserve(unsorted.size());
    for (size_t index : BuildMaterialDrawOrder(sortKeys))
    {
        ordered.push_back(std::move(unsorted[index]));
    }
    return ordered;
}

void VulkanRenderer::AppendDrawItem(
    const RenderSubmesh& renderSubmesh,
    const glm::mat4& model,
    const ViewFrustum& frustum,
    const glm::mat4& view,
    std::vector<VulkanDrawItem>& items,
    std::vector<MaterialDrawSortKey>& sortKeys)
{
    const glm::vec3 worldCenter = glm::vec3(model * glm::vec4(renderSubmesh.localBoundsCenter, 1.0f));
    const float worldRadius =
        renderSubmesh.localBoundsRadius *
        std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])), glm::length(glm::vec3(model[2]))});
    if (!frustum.IntersectsSphere(worldCenter, worldRadius))
    {
        return;
    }
    // The view is rigid, so the view-space centre's length is the camera's distance to it.
    const glm::vec4 viewCenter = view * glm::vec4(worldCenter, 1.0f);
    if (!renderSubmesh.drawDistance.Contains(glm::length(glm::vec3(viewCenter))))
    {
        return;
    }
    ObjectPushConstants drawConstants{};
    drawConstants.model = model;
    const MaterialPipelineKey pipelineKey{
        renderSubmesh.alphaMode,
        renderSubmesh.doubleSided};
    // A toon material's opaque draw is the geometry pass's like any deferred one (its forward flag
    // keeps the lighting pass off it), and the toon passes shade it; triangle.frag never does.
    const bool forwardShaded = !renderSubmesh.toon && renderSubmesh.alphaMode != MaterialAlphaMode::Blend &&
                               (renderSubmesh.material.shadingModel[0] & kShadingFlagForward) != 0u;
    const bool transmissive = forwardShaded && (renderSubmesh.material.shadingModel[0] & kShadingFlagTransmission) != 0u;
    sortKeys.push_back({pipelineKey, -viewCenter.z, forwardShaded, transmissive});
    VulkanDrawItem& item = items.emplace_back(VulkanDrawItem{
        renderSubmesh.buffer->GetVertexBuffer(),
        renderSubmesh.buffer->GetIndexBuffer(),
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
        renderSubmesh.decal,
        renderSubmesh.toon.get(),
        renderSubmesh.entity,
        renderSubmesh.toonHeadJoint,
        renderSubmesh.buffer->GetPreviousPositionBuffer()});
    std::copy(std::begin(renderSubmesh.material.samplerIndices), std::end(renderSubmesh.material.samplerIndices), item.samplerIndices.begin());
}

std::vector<ShadowDrawItem> VulkanRenderer::BuildSelectionDrawItems(
    entt::entity selected,
    std::span<const glm::mat4> models,
    const glm::mat4& viewProjection,
    const glm::vec3& cameraPosition) const
{
    std::vector<ShadowDrawItem> items;
    if (selected == entt::null)
    {
        return items;
    }
    const ViewFrustum frustum(viewProjection);
    for (size_t submeshIndex = 0; submeshIndex < m_renderSubmeshes.size(); ++submeshIndex)
    {
        const RenderSubmesh& renderSubmesh = *m_renderSubmeshes[submeshIndex];
        // A decal is a box projected onto what is under it; its own shape is not the entity's. A level
        // of detail the camera does not draw is not part of what is seen either.
        if (renderSubmesh.entity != selected || renderSubmesh.decal ||
            !WithinDrawDistance(renderSubmesh, models[submeshIndex], cameraPosition))
        {
            continue;
        }
        // As a caster, but Blend surfaces (glass) too: they are part of the silhouette. Only Mask
        // cutouts are tested.
        ShadowDrawItem item{};
        FillShadowDrawItem(renderSubmesh, models[submeshIndex], item);
        if (frustum.IntersectsSphere(item.worldBoundsCenter, item.worldBoundsRadius))
        {
            items.push_back(item);
        }
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

std::vector<ShadowDrawItem> VulkanRenderer::BuildShadowDrawItems(
    uint32_t imageIndex,
    std::span<const glm::mat4> models,
    const glm::vec3& cameraPosition) const
{
    // Opaque casters first, then alpha-tested ones, so the pass switches pipeline once. On the task
    // system in chunks: each chunk counts its casters of either kind, which places them in the list,
    // then writes them there, in the order one loop over every submesh would give.
    const uint32_t chunkCount = (static_cast<uint32_t>(m_renderSubmeshes.size()) + kSubmeshesPerTask - 1) / kSubmeshesPerTask;
    const auto forEachChunk = [&](const auto& body)
    {
        TaskSystem::ParallelFor(chunkCount, 1, [&](uint32_t firstChunk, uint32_t endChunk)
                                {
                                    for (uint32_t chunk = firstChunk; chunk < endChunk; ++chunk)
                                    {
                                        const size_t begin = static_cast<size_t>(chunk) * kSubmeshesPerTask;
                                        body(chunk, begin, std::min(begin + kSubmeshesPerTask, m_renderSubmeshes.size()));
                                    }
                                });
    };
    std::vector<uint32_t> opaqueCounts(chunkCount, 0);
    std::vector<uint32_t> maskedCounts(chunkCount, 0);
    forEachChunk([&](uint32_t chunk, size_t begin, size_t end)
                 {
                     for (size_t submeshIndex = begin; submeshIndex < end; ++submeshIndex)
                     {
                         const ShadowCaster caster = ClassifyShadowCaster(*m_renderSubmeshes[submeshIndex], models[submeshIndex], cameraPosition);
                         if (caster == ShadowCaster::Opaque)
                         {
                             ++opaqueCounts[chunk];
                         }
                         else if (caster == ShadowCaster::Masked)
                         {
                             ++maskedCounts[chunk];
                         }
                     }
                 });
    std::vector<uint32_t> opaqueFirst(chunkCount);
    std::vector<uint32_t> maskedFirst(chunkCount);
    uint32_t count = 0;
    for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
    {
        opaqueFirst[chunk] = count;
        count += opaqueCounts[chunk];
    }
    for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
    {
        maskedFirst[chunk] = count;
        count += maskedCounts[chunk];
    }
    std::vector<ShadowDrawItem> items(count);
    forEachChunk([&](uint32_t chunk, size_t begin, size_t end)
                 {
                     uint32_t nextOpaque = opaqueFirst[chunk];
                     uint32_t nextMasked = maskedFirst[chunk];
                     for (size_t submeshIndex = begin; submeshIndex < end; ++submeshIndex)
                     {
                         const RenderSubmesh& renderSubmesh = *m_renderSubmeshes[submeshIndex];
                         const ShadowCaster caster = ClassifyShadowCaster(renderSubmesh, models[submeshIndex], cameraPosition);
                         if (caster != ShadowCaster::None)
                         {
                             FillShadowDrawItem(renderSubmesh, models[submeshIndex], items[caster == ShadowCaster::Opaque ? nextOpaque++ : nextMasked++]);
                         }
                     }
                 });
    return items;
}

VulkanRenderer::ShadowCaster VulkanRenderer::ClassifyShadowCaster(
    const RenderSubmesh& renderSubmesh,
    const glm::mat4& model,
    const glm::vec3& cameraPosition)
{
    // Blend materials are glass, foliage cards and the like; a solid shadow from them would be
    // wrong more often than none, so they cast none.
    // Transmissive surfaces let most light through; they cast none either. Nor does a submesh its
    // model says casts none, or one the camera is too near or too far from to draw.
    if (renderSubmesh.alphaMode == MaterialAlphaMode::Blend ||
        (renderSubmesh.material.shadingModel[0] & kShadingFlagTransmission) != 0u || !renderSubmesh.castShadows ||
        !WithinDrawDistance(renderSubmesh, model, cameraPosition))
    {
        return ShadowCaster::None;
    }
    return renderSubmesh.alphaMode == MaterialAlphaMode::Mask ? ShadowCaster::Masked : ShadowCaster::Opaque;
}

bool VulkanRenderer::WithinDrawDistance(const RenderSubmesh& renderSubmesh, const glm::mat4& model, const glm::vec3& cameraPosition)
{
    if (!renderSubmesh.drawDistance.IsLimited())
    {
        return true;
    }
    const glm::vec3 worldCenter = glm::vec3(model * glm::vec4(renderSubmesh.localBoundsCenter, 1.0f));
    return renderSubmesh.drawDistance.Contains(glm::distance(worldCenter, cameraPosition));
}

void VulkanRenderer::FillShadowDrawItem(const RenderSubmesh& renderSubmesh, const glm::mat4& model, ShadowDrawItem& item)
{
    item.vertexBuffer = renderSubmesh.buffer->GetVertexBuffer();
    item.positionBuffer = renderSubmesh.buffer->GetPositionBuffer();
    item.indexBuffer = renderSubmesh.buffer->GetIndexBuffer();
    item.indexCount = renderSubmesh.buffer->GetIndexCount();
    item.model = model;
    item.worldBoundsCenter = glm::vec3(item.model * glm::vec4(renderSubmesh.localBoundsCenter, 1.0f));
    // The largest axis scale keeps the sphere enclosing under non-uniform scale.
    item.worldBoundsRadius =
        renderSubmesh.localBoundsRadius *
        std::max({glm::length(glm::vec3(item.model[0])),
                  glm::length(glm::vec3(item.model[1])),
                  glm::length(glm::vec3(item.model[2]))});
    item.alphaMask = renderSubmesh.alphaMode == MaterialAlphaMode::Mask;
    item.materialSet = renderSubmesh.materialSet;
    item.drawSlot = renderSubmesh.drawSlot;
    std::memcpy(item.material.baseColorFactor, renderSubmesh.material.baseColorFactor, sizeof(item.material.baseColorFactor));
    std::memcpy(item.material.nodeGraphFactors, renderSubmesh.material.nodeGraphFactors, sizeof(item.material.nodeGraphFactors));
    item.material.alphaCutoff = renderSubmesh.material.alphaCutoff;
    // The alpha test samples the base colour where the main passes do.
    std::memcpy(item.baseColorTransform, &renderSubmesh.textureTransforms.rows[0], sizeof(item.baseColorTransform));
}

void VulkanRenderer::RecordTransitions(VulkanSceneView& view, const RenderPassIo& io, const ScenePassFrameContext& frame)
{
    // What the pass reads as shader resources, what it writes in its write state (render_target_layout.h).
    // NVRHI knows where each target is, and a write after a write in unordered access gets its UAV
    // barrier.
    nvrhi::ICommandList* commandList = frame.commandList;
    const auto texture = [&](RenderTargetId target)
    {
        return view.targets->GetTexture(target, view.targets->ResolveIndex(target, frame.imageIndex, frame.frameSlot));
    };
    for (const RenderTargetId target : io.reads)
    {
        commandList->setTextureState(texture(target), nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    }
    for (const RenderTargetId target : io.writes)
    {
        commandList->setTextureState(texture(target), nvrhi::AllSubresources, GetWriteState(target));
    }
    commandList->commitBarriers();
}

void VulkanRenderer::UpdatePathTracing(
    VulkanSceneView& sceneView,
    const ViewportMatrices& matrices,
    const RenderDebugSettings& settings,
    bool viewport,
    ScenePassFrameContext& frame,
    const RenderFramePacket& packet,
    std::span<const GpuLightData> lights,
    const glm::vec3& ambientLuminance,
    const EnvironmentUniformData& environment,
    float preExposure)
{
    uint32_t stillFrames = 0;
    // The plain path tracer: not while ReSTIR PT runs in its place (its bookkeeping is in the frame setup).
    // The forward-shaded surfaces' layer, which either traces, keeps its history the same way.
    const bool plainPathTracing = frame.pathTracing.enabled && !frame.pathTracing.restir;
    sceneView.offlineProgress.reset();
    if (plainPathTracing || frame.pathTraceLayer)
    {
        if (plainPathTracing && sceneView.pathTracePass->Prepare(*sceneView.targets))
        {
            sceneView.pathTraceHistory.Reset();
            sceneView.pathTraceAccumulation.Reset();
        }
        // The light as the paths see it: every selected light, the ambient, and the sky's sun, air and
        // HDRI. The clouds drift every frame and are left out, so a still image keeps averaging them.
        std::vector<glm::vec4> lighting;
        lighting.reserve(lights.size() * 5 + 9);
        for (const GpuLightData& light : lights)
        {
            lighting.insert(lighting.end(), {light.positionAndRange, light.colorAndIntensity, light.directionAndType, light.spotAndArea, light.areaRightAxis});
        }
        lighting.insert(
            lighting.end(),
            {glm::vec4(ambientLuminance, 0.0f), environment.sunDirectionAndMode, environment.sunIlluminance, environment.rayleighScattering,
             environment.mieParameters, environment.ozoneAbsorption, environment.groundAlbedo, environment.hdriParameters,
             environment.hdriIrradianceSh[0]});
        PathTraceView view;
        view.view = matrices.view;
        view.projection = matrices.projection;
        view.width = frame.extent.width;
        view.height = frame.extent.height;
        const bool sceneChanged =
            packet.contentChanged || m_ddgiMovingInstances.MovedThisFrame() || sceneView.pathTraceGeometryEpoch != m_ddgiGeometryEpoch;
        stillFrames = sceneView.pathTraceAccumulation.Advance(view, lighting, settings, sceneChanged);
        frame.pathTraceHistoryCap = PathTraceHistoryCap(frame.pathTracing, stillFrames);
        // The offline image stops tracing once a still image has its samples.
        if (plainPathTracing && frame.pathTracing.offline.enabled)
        {
            sceneView.offlineProgress = OfflinePathTraceProgress(frame.pathTracing.offline, stillFrames);
            frame.pathTraceHold = sceneView.offlineProgress->done;
        }
    }
    else
    {
        sceneView.pathTraceAccumulation.Reset();
    }
    sceneView.pathTraceGeometryEpoch = m_ddgiGeometryEpoch;
    frame.pathTraceHistory = sceneView.pathTraceHistory.Advance(plainPathTracing && frame.pathTracing.accumulate);
    frame.pathTraceHistoryScale = TaaHistoryScale(frame.pathTraceHistory.valid, preExposure, sceneView.pathTraceHistoryPreExposure);
    sceneView.pathTraceHistoryPreExposure = preExposure;
    // The Graphics Debug status line is the viewport's.
    if (!viewport)
    {
        return;
    }

    const RenderDebugSettings& renderDebug = packet.renderDebug;
    m_pathTracingProgress = -1.0f;
    if (!renderDebug.pathTracing.enabled)
    {
        m_pathTracingStatus.clear();
    }
    else if (renderDebug.pathTracing.restir && frame.pathTracing.restir)
    {
        const RestirPtSettings& restirPt = frame.pathTracing.restirPt;
        m_pathTracingStatus = frame.dlssRayReconstruction ? "ReSTIR PT: DLSS ray reconstruction denoises it"
                              : restirPt.accumulate       ? std::format("ReSTIR PT: {} still frames averaged", frame.restirPtAccumulatedFrames)
                                                          : "ReSTIR PT: best with DLSS ray reconstruction";
    }
    else if (m_view.pathTracePass == nullptr || !m_view.pathTracePass->IsSupported())
    {
        m_pathTracingStatus = "Needs hardware ray tracing: the GPU has no ray queries";
    }
    else if (!frame.pathTracing.enabled)
    {
        m_pathTracingStatus = !renderDebug.hardwareRayTracing ? "Off: hardware ray tracing is switched off"
                              : renderDebug.forwardOnly       ? "Off in the forward-only order"
                              : renderDebug.khronosReference  ? "Off in the Khronos reference view"
                                                              : "Waiting for the ray scene";
    }
    else if (frame.pathTracing.offline.enabled)
    {
        // How far the offline image is, timed from its first still frame.
        const OfflinePathTracingSettings& offline = frame.pathTracing.offline;
        const OfflineProgress progress = OfflinePathTraceProgress(offline, stillFrames);
        const auto now = std::chrono::steady_clock::now();
        if (stillFrames == 0u)
        {
            m_offlineStart = now;
            m_offlineSeconds.reset();
        }
        const double seconds = std::chrono::duration<double>(now - m_offlineStart).count();
        if (progress.done && !m_offlineSeconds)
        {
            m_offlineSeconds = seconds;
        }
        const char* const denoiser = frame.dlssRayReconstruction ? "ray reconstruction" : "own filter";
        if (progress.done)
        {
            m_pathTracingStatus = std::format("Offline: done, {} spp in {:.1f} s ({})", progress.targetSamples, m_offlineSeconds.value_or(seconds), denoiser);
        }
        else if (progress.targetSamples == 0u)
        {
            m_pathTracingStatus = std::format("Offline: {} spp, {:.1f} s, no target ({})", progress.samples, seconds, denoiser);
        }
        else
        {
            const double left = seconds * (static_cast<double>(progress.targetSamples) / static_cast<double>(std::max(progress.samples, 1u)) - 1.0);
            m_pathTracingStatus = stillFrames == 0u ? std::format("Offline: {} spp a frame, waiting for a still image ({})",
                                                                  OfflineSamplesPerPixel(offline), denoiser)
                                                    : std::format("Offline: {} / {} spp, {:.1f} s, about {:.0f} s left ({})", progress.samples,
                                                                  progress.targetSamples, seconds, left, denoiser);
        }
        m_pathTracingProgress =
            progress.targetSamples == 0u ? -1.0f : std::min(static_cast<float>(progress.samples) / static_cast<float>(progress.targetSamples), 1.0f);
        return;
    }
    else if (frame.dlssRayReconstruction)
    {
        m_pathTracingStatus = "DLSS ray reconstruction denoises the paths";
    }
    else if (!frame.pathTracing.accumulate)
    {
        m_pathTracingStatus = "One frame of paths (accumulation off)";
    }
    else
    {
        // A pixel's history grows by one each still frame from what it held when the image stopped
        // changing, so the still frames are how far the reference has come.
        const uint32_t maxFrames = PathTraceHistoryCap(frame.pathTracing, kPathTraceMaxFrames);
        m_pathTracingStatus = stillFrames >= maxFrames ? std::format("Converged: {} frames averaged", maxFrames)
                              : stillFrames > 0u       ? std::format("Still for {} of {} frames", stillFrames, maxFrames)
                                                       : std::format("Moving: up to {} frames averaged", frame.pathTraceHistoryCap);
    }
}

void VulkanRenderer::RecordScenePasses(
    VkCommandBuffer commandBuffer,
    VulkanSceneView& view,
    const ScenePassFrameContext& frame,
    std::span<const ScenePassId> passOrder,
    VulkanGpuTimer* timer)
{
    // Only the passes in this frame's order record, but every owned pass follows a target rebuild
    // (see RebuildViewTargets), so flipping the switch never meets a stale framebuffer.
    for (const ScenePassId id : passOrder)
    {
        const IScenePass* pass = view.FindPass(id);
        RecordTransitions(view, pass->Io(), frame);
        pass->Record(commandBuffer, *view.targets, frame);
        if (timer != nullptr)
        {
            timer->Mark(ScenePassName(id));
        }
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

glm::mat3 VulkanRenderer::UpdateWhiteBalance(RenderFramePacket& frame)
{
    Camera& camera = frame.camera;
    const AutoWhiteBalanceSettings& settings = camera.autoWhiteBalance;
    if (!settings.enabled || frame.renderDebug.khronosReference)
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
    if (m_view.exposurePass != nullptr)
    {
        m_whiteBalanceReferences.frameColorRgb = m_view.exposurePass->GetFrameColor(m_commandContext->GetCurrentFrame());
    }
    const glm::vec2 target = EstimateIlluminantXy(m_whiteBalanceReferences);
    m_adaptedWhiteXy = m_adaptedWhiteXy.has_value()
                           ? AdaptWhitePointXy(*m_adaptedWhiteXy, target, frame.deltaSeconds, settings.adaptPerSecond)
                           : target;
    camera.adaptedWhiteKelvin = CorrelatedColorTemperature(*m_adaptedWhiteXy);
    return WhiteBalanceMatrix(*m_adaptedWhiteXy, settings.degree, DaylightXy(settings.targetKelvin));
}

void VulkanRenderer::UpdateAutoExposure(VulkanSceneView& view, Camera& camera, const RenderFramePacket& frame, uint32_t frameSlot)
{
    // The scene's own stops (a white studio) on top of the camera's.
    AutoExposureSettings settings = camera.autoExposure;
    settings.compensationEv += frame.environment.exposureCompensationEv;
    // Whatever sets the EV below, the next frame adapts from it.
    struct RememberExposure
    {
        std::optional<float>& remembered;
        const Camera& camera;
        ~RememberExposure()
        {
            remembered = camera.exposureEv100;
        }
    } rememberExposure{view.exposureEv100, camera};
    if (frame.renderDebug.khronosReference)
    {
        // The Sample Viewer's exposure 1.0: an HDRI texel of 1 exposed to 1. Without an HDRI the
        // exposure is left where it is.
        if (frame.environment.mode == EnvironmentMode::Hdri)
        {
            camera.exposureEv100 = KhronosReferenceEv100(frame.environment.hdri.intensity);
        }
        else if (view.exposureEv100.has_value())
        {
            camera.exposureEv100 = *view.exposureEv100;
        }
        return;
    }
    if (!settings.enabled || !view.exposurePass)
    {
        // Manual mode: exposureEv100 is the user's, as the frame brought it. When auto exposure is
        // turned back on it adapts from that value rather than snapping.
        return;
    }
    // The main thread's camera trails the adapted EV by a frame: adapt from where this thread got to
    // (a capture view's starts from the viewport camera's, its first frame).
    if (view.exposureEv100.has_value())
    {
        camera.exposureEv100 = *view.exposureEv100;
    }

    // The slot's fence has signaled, so its histogram is the one it recorded kMaxFramesInFlight
    // frames ago. Empty means nothing but background was visible; the exposure then holds.
    const std::span<const uint32_t> histogram = view.exposurePass->GetHistogram(frameSlot);
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
        view.autoExposureState,
        camera.exposureEv100,
        *target,
        MeterLongTermTargetEv100(references, settings),
        frame.deltaSeconds,
        settings);
    camera.adaptedLongTermEv100 = view.autoExposureState.longTermEv100;
    view.hasMeteredExposure = true;
}

void VulkanRenderer::RecordEditorLayer(nvrhi::ICommandList* commandList, uint32_t imageIndex, uint32_t frameSlot, ImDrawData* drawData) const
{
    // The swapchain image comes from the presentation engine in no state worth keeping (Vulkan:
    // UNDEFINED; D3D12: PRESENT); it is cleared, drawn into, and handed back for presenting.
    nvrhi::ITexture* backBuffer = m_backBuffers.at(imageIndex);
    commandList->clearState();
    commandList->beginTrackingTextureState(
        backBuffer,
        nvrhi::AllSubresources,
        m_nvrhi->Get()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? nvrhi::ResourceStates::Common : nvrhi::ResourceStates::Present);
    ClearTextureFloat(commandList, backBuffer, nvrhi::Color(0.04f, 0.05f, 0.08f, 1.0f));
    commandList->setTextureState(backBuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->commitBarriers();
    m_imguiLayer->GetRenderer().Render(commandList, m_backBufferFramebuffers.at(imageIndex), drawData, frameSlot, m_swapchain->IsHdr());
    // MINIENGINE_CAPTURE_WINDOW: the whole window as presented, which CaptureViewportNow writes too.
    static const char* const windowCapture = std::getenv("MINIENGINE_CAPTURE_WINDOW");
    if (windowCapture != nullptr && windowCapture[0] != 0)
    {
        const nvrhi::TextureDesc& desc = backBuffer->getDesc();
        if (!m_windowCaptureStaging || m_windowCaptureStaging->getDesc().width != desc.width ||
            m_windowCaptureStaging->getDesc().height != desc.height || m_windowCaptureStaging->getDesc().format != desc.format)
        {
            nvrhi::TextureDesc stagingDesc;
            stagingDesc.width = desc.width;
            stagingDesc.height = desc.height;
            stagingDesc.format = desc.format;
            stagingDesc.debugName = "Window capture";
            m_windowCaptureStaging = m_nvrhi->Get()->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        }
        commandList->setTextureState(backBuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
        commandList->commitBarriers();
        commandList->copyTexture(m_windowCaptureStaging, nvrhi::TextureSlice(), backBuffer, nvrhi::TextureSlice());
    }
    commandList->setTextureState(backBuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
    commandList->commitBarriers();
    commandList->clearState();
}
}
