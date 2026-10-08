#include "path_trace_pass.h"

#include "gpu_timer.h"
#include "path_trace_layer_pass.h"
#include "ray_scene.h"

#include <algorithm>
#include <array>

namespace me
{

namespace
{
// Must match PathTraceConstants in shaders/vulkan/path_trace_common.glsl.
struct PathTracePushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    uint32_t frameIndex = 0;
    uint32_t flags = 0;
    uint32_t maxBounces = 0;
    uint32_t lightCandidates = 0;
    float fireflyClamp = 0.0f;
    float historyScale = 1.0f;
    float historyCap = 1.0f;
    float motionFrames = 1.0f;
    uint32_t stepSize = 0;
    uint32_t source = 0;
    uint32_t target = 0;
    uint32_t unused = 0;
};
static_assert(sizeof(PathTracePushConstants) == 64, "PathTracePushConstants must match path_trace_common.glsl");

// Must match the PT_FLAG_* and PT_IMAGE_* constants in path_trace_common.glsl.
constexpr uint32_t kFlagAccumulate = 1u;
constexpr uint32_t kFlagDenoise = 2u;
constexpr uint32_t kFlagHistoryValid = 4u;
constexpr uint32_t kFlagRayMedia = 8u;
constexpr uint32_t kFlagForwardSurfaces = 16u;
constexpr uint32_t kImageRaw = 0u;
constexpr uint32_t kImageHistory = 1u;
constexpr uint32_t kImageFinal = 2u;

constexpr VkFormat kImageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kBindingCount = 19;
constexpr uint32_t kSampledBindings = 12;
constexpr uint32_t kStorageBindings = 7;
// Bounces and light candidates a path may be given at most.
constexpr int kMaxBounces = 16;
constexpr int kMaxLightCandidates = 32;

void ComputeBarrier(VkCommandBuffer commandBuffer)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void Dispatch(VkCommandBuffer commandBuffer, VkExtent2D extent)
{
    vkCmdDispatch(
        commandBuffer,
        (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        1);
}
}

VulkanPathTracePass::VulkanPathTracePass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanRayScene& rayScene,
    TextureDescriptorBinding multiScattering)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_multiScattering(multiScattering)
{
    if (!rayScene.HasHardwareRayTracing())
    {
        return;
    }
    try
    {
        m_nearestSampler = CreateClampSampler(m_device, VK_FILTER_NEAREST);
        std::array<VkDescriptorType, kBindingCount> types{};
        for (uint32_t binding = 0; binding < kBindingCount; ++binding)
        {
            const bool storage = (binding >= 8 && binding <= 13) || binding == 17;
            types[binding] = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        m_setLayout = CreateComputeSetLayout(m_device, types);
        const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, m_setLayout};
        CreateComputePipeline(
            m_device, pipelineCache, setLayouts, "path_trace_filter.comp.spv", sizeof(PathTracePushConstants), m_pipelineLayout, m_filterPipeline);
        m_temporalPipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "path_trace_temporal.comp.spv");
        const std::array<VkDescriptorSetLayout, 4> traceLayouts = {
            frameSetLayout, rayScene.GetSetLayout(), m_setLayout, rayScene.GetTextureSetLayout()};
        CreateComputePipeline(
            m_device, pipelineCache, traceLayouts, "path_trace.comp.spv", sizeof(PathTracePushConstants), m_tracePipelineLayout, m_tracePipeline);
        // The plain path tracer's sets, by transient copy and history read index; the layer's two.
        const uint32_t setCount = targets.GetTransientCopyCount() * 2;
        m_descriptorPool = CreateImageDescriptorPool(m_device, setCount + 2, kSampledBindings, kStorageBindings);
        m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, setCount + 2);
        m_layerDescriptorSets.assign(m_descriptorSets.end() - 2, m_descriptorSets.end());
        m_descriptorSets.resize(setCount);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanPathTracePass::~VulkanPathTracePass()
{
    DestroyHandles();
}

ScenePassId VulkanPathTracePass::Id() const
{
    return ScenePassId::PathTrace;
}

RenderPassIo VulkanPathTracePass::Io() const
{
    static constexpr std::array<RenderTargetId, 8> kReads = {
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferCoat,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::GBufferSpecular,
        RenderTargetId::GBufferSheen};
    // Written only in path tracing mode; otherwise the SSR resolve's reflections stand, and the GI
    // resolve writes the indirect diffuse after lighting as before.
    static constexpr std::array<RenderTargetId, 2> kWrites = {RenderTargetId::SceneGi, RenderTargetId::SceneReflections};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

bool VulkanPathTracePass::IsSupported() const
{
    return m_tracePipeline != VK_NULL_HANDLE;
}

void VulkanPathTracePass::CreateRaw(VkExtent2D extent)
{
    if (!m_rawReady)
    {
        m_raw.Create(m_physicalDevice, m_device, extent, kImageFormat);
        m_rawReady = true;
    }
}

bool VulkanPathTracePass::Prepare(const SceneRenderTargets& targets)
{
    if (m_imagesReady || !IsSupported())
    {
        return false;
    }
    try
    {
        const VkExtent2D extent = targets.GetExtent();
        CreateRaw(extent);
        m_diffuseHistory.Create(m_physicalDevice, m_device, extent, kImageFormat);
        m_specularHistory.Create(m_physicalDevice, m_device, extent, kImageFormat);
        m_surfaceHistory.Create(m_physicalDevice, m_device, extent, kImageFormat);
    }
    catch (...)
    {
        DestroyImages();
        throw;
    }
    WriteDescriptorSets(targets);
    m_imagesReady = true;
    return true;
}

bool VulkanPathTracePass::PrepareLayer(const SceneRenderTargets& targets, const VulkanPathTraceLayerPass& layer)
{
    if (m_layerReady || !IsSupported() || !layer.IsReady())
    {
        return false;
    }
    try
    {
        const VkExtent2D extent = targets.GetExtent();
        CreateRaw(extent);
        m_layerDiffuseHistory.Create(m_physicalDevice, m_device, extent, kImageFormat);
        m_layerSpecularHistory.Create(m_physicalDevice, m_device, extent, kImageFormat);
        m_layerSurfaceHistory.Create(m_physicalDevice, m_device, extent, kImageFormat);
        m_layerResult.Create(m_physicalDevice, m_device, extent, kImageFormat);
    }
    catch (...)
    {
        DestroyImages();
        throw;
    }
    WriteLayerDescriptorSets(layer);
    m_layerReady = true;
    m_layerInitialized = false;
    return true;
}

bool VulkanPathTracePass::IsLayerReady() const
{
    return m_layerReady;
}

void VulkanPathTracePass::RecordLayerInitialTransition(VkCommandBuffer commandBuffer) const
{
    if (m_layerInitialized || !m_layerReady)
    {
        return;
    }
    std::array<VkImageMemoryBarrier, 2> barriers{};
    for (uint32_t index = 0; index < 2; ++index)
    {
        VkImageMemoryBarrier& barrier = barriers[index];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_layerResult.GetImage(index);
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<uint32_t>(barriers.size()),
        barriers.data());
    m_layerInitialized = true;
}

TextureDescriptorBinding VulkanPathTracePass::GetLayerDiffuseBinding() const
{
    return TextureDescriptorBinding{m_layerResult.GetView(0), m_nearestSampler};
}

TextureDescriptorBinding VulkanPathTracePass::GetLayerSpecularBinding() const
{
    return TextureDescriptorBinding{m_layerResult.GetView(1), m_nearestSampler};
}

void VulkanPathTracePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    if (!frame.pathTracing.enabled)
    {
        return;
    }
    const PathTracingSettings& settings = frame.pathTracing;
    // ReSTIR PT (restir_pt_pass.h) runs in the plain path tracer's place when pathTracing.restir is set.
    if (!settings.restir && m_imagesReady)
    {
        const bool historyValid = settings.accumulate && frame.pathTraceHistory.valid;
        // The raw paths are rewritten whole; the histories keep last frame's contents where they are valid.
        m_raw.RecordBarrier(commandBuffer, false);
        m_diffuseHistory.RecordBarrier(commandBuffer, historyValid);
        m_specularHistory.RecordBarrier(commandBuffer, historyValid);
        m_surfaceHistory.RecordBarrier(commandBuffer, historyValid);
        const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneGi, frame.imageIndex, frame.frameSlot);
        RecordPaths(
            commandBuffer, frame, m_descriptorSets.at(slot * 2 + frame.pathTraceHistory.readIndex), settings.accumulate, settings.denoise,
            historyValid, frame.pathTraceHistoryScale);
        // Its own GPU timer section; the renderer's mark after the pass closes the layer's.
        if (frame.pathTraceLayer && m_layerReady && frame.gpuTimer != nullptr)
        {
            frame.gpuTimer->Mark(commandBuffer, "PathTraceOpaque");
        }
    }

    if (frame.pathTraceLayer && m_layerReady)
    {
        RecordLayerInitialTransition(commandBuffer);
        const bool historyValid = frame.pathTraceLayerAccumulate && frame.pathTraceLayerHistory.valid;
        // The raw pair again, after the plain path tracer's last read of it.
        m_raw.RecordBarrier(commandBuffer, false);
        m_layerDiffuseHistory.RecordBarrier(commandBuffer, historyValid);
        m_layerSpecularHistory.RecordBarrier(commandBuffer, historyValid);
        m_layerSurfaceHistory.RecordBarrier(commandBuffer, historyValid);
        // The result, rewritten whole: from where last frame's forward pass sampled it to the stores,
        // and back once it is written.
        const auto resultBarrier = [&](bool written)
        {
            std::array<VkImageMemoryBarrier, 2> barriers{};
            for (uint32_t index = 0; index < 2; ++index)
            {
                VkImageMemoryBarrier& barrier = barriers[index];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = written ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = m_layerResult.GetImage(index);
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                barrier.srcAccessMask = written ? VK_ACCESS_SHADER_WRITE_BIT : 0;
                barrier.dstAccessMask = written ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(
                commandBuffer,
                written ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                written ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0,
                nullptr,
                0,
                nullptr,
                static_cast<uint32_t>(barriers.size()),
                barriers.data());
        };
        resultBarrier(false);
        RecordPaths(
            commandBuffer, frame, m_layerDescriptorSets.at(frame.pathTraceLayerHistory.readIndex), frame.pathTraceLayerAccumulate,
            frame.pathTraceLayerDenoise, historyValid, frame.pathTraceLayerHistoryScale);
        resultBarrier(true);
    }
}

void VulkanPathTracePass::RecordPaths(
    VkCommandBuffer commandBuffer,
    const ScenePassFrameContext& frame,
    VkDescriptorSet passSet,
    bool accumulate,
    bool denoise,
    bool historyValid,
    float historyScale) const
{
    const PathTracingSettings& settings = frame.pathTracing;
    PathTracePushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.frameIndex = frame.frameIndex;
    constants.flags = (accumulate ? kFlagAccumulate : 0u) | (denoise ? kFlagDenoise : 0u) | (historyValid ? kFlagHistoryValid : 0u) |
                      (settings.rayMedia ? kFlagRayMedia : 0u) | (settings.forwardSurfaces ? kFlagForwardSurfaces : 0u);
    constants.maxBounces = static_cast<uint32_t>(std::clamp(settings.maxBounces, 0, kMaxBounces));
    constants.lightCandidates = static_cast<uint32_t>(std::clamp(settings.lightCandidates, 0, kMaxLightCandidates));
    constants.fireflyClamp = std::max(settings.fireflyClamp, 0.0f);
    constants.historyScale = historyScale;
    constants.historyCap = static_cast<float>(std::max(frame.pathTraceHistoryCap, 1u));
    constants.motionFrames = static_cast<float>(std::max(settings.motionFrames, 1));

    const std::array<VkDescriptorSet, 4> traceSets = {frame.frameDescriptorSet, frame.raySet, passSet, frame.rayTextureSet};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeline);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipelineLayout, 0, static_cast<uint32_t>(traceSets.size()), traceSets.data(), 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_tracePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    Dispatch(commandBuffer, frame.extent);
    if (!accumulate && !denoise)
    {
        return;
    }
    ComputeBarrier(commandBuffer);

    const std::array<VkDescriptorSet, 2> sets = {frame.frameDescriptorSet, passSet};
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    if (accumulate)
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipeline);
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        Dispatch(commandBuffer, frame.extent);
        ComputeBarrier(commandBuffer);
    }

    // Three a-trous iterations, the raw pair as scratch between them, the last into the targets; or
    // the accumulation copied there as it is.
    struct Iteration
    {
        uint32_t stepSize;
        uint32_t source;
        uint32_t target;
    };
    const uint32_t input = accumulate ? kImageHistory : kImageRaw;
    const std::array<Iteration, 3> filtered = {Iteration{1u, input, kImageFinal}, Iteration{2u, kImageFinal, kImageRaw}, Iteration{4u, kImageRaw, kImageFinal}};
    const std::array<Iteration, 1> copied = {Iteration{0u, input, kImageFinal}};
    const std::span<const Iteration> iterations = denoise ? std::span<const Iteration>(filtered) : std::span<const Iteration>(copied);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_filterPipeline);
    for (size_t index = 0; index < iterations.size(); ++index)
    {
        if (index > 0)
        {
            ComputeBarrier(commandBuffer);
        }
        constants.stepSize = iterations[index].stepSize;
        constants.source = iterations[index].source;
        constants.target = iterations[index].target;
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        Dispatch(commandBuffer, frame.extent);
    }
}

void VulkanPathTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // Remade at the new size by the next path traced frame's Prepare, which then resets the history.
    (void)targets;
    DestroyImages();
}

void VulkanPathTracePass::WriteDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const VkDescriptorSet set = m_descriptorSets.at(slot * 2 + readIndex);
            const uint32_t writeIndex = 1u - readIndex;
            const auto sampled = [&](RenderTargetId target)
            {
                return VkDescriptorImageInfo{m_nearestSampler, targets.GetSampledView(target, slot), kReadLayout};
            };
            const auto storage = [](VkImageView view)
            {
                return VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            };
            const auto history = [&](const HistoryImagePair& pair)
            {
                return VkDescriptorImageInfo{m_nearestSampler, pair.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
            };
            const std::array<VkDescriptorImageInfo, kBindingCount> infos = {
                sampled(RenderTargetId::SceneDepth),
                sampled(RenderTargetId::GBufferNormal),
                sampled(RenderTargetId::GBufferAlbedo),
                sampled(RenderTargetId::GBufferSurface),
                sampled(RenderTargetId::GBufferCoat),
                sampled(RenderTargetId::GBufferVelocity),
                sampled(RenderTargetId::GBufferSpecular),
                sampled(RenderTargetId::GBufferSheen),
                storage(m_raw.GetView(0)),
                storage(m_raw.GetView(1)),
                storage(m_diffuseHistory.GetView(writeIndex)),
                storage(m_specularHistory.GetView(writeIndex)),
                storage(targets.GetView(RenderTargetId::SceneGi, slot)),
                storage(targets.GetView(RenderTargetId::SceneReflections, slot)),
                history(m_diffuseHistory),
                history(m_specularHistory),
                history(m_surfaceHistory),
                storage(m_surfaceHistory.GetView(writeIndex)),
                VkDescriptorImageInfo{m_multiScattering.sampler, m_multiScattering.imageView, VK_IMAGE_LAYOUT_GENERAL}};
            std::array<VkWriteDescriptorSet, kBindingCount> writes{};
            for (uint32_t binding = 0; binding < kBindingCount; ++binding)
            {
                const VkDescriptorType type = infos[binding].sampler != VK_NULL_HANDLE ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                                       : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[binding] = ImageWrite(set, binding, type, &infos[binding]);
            }
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void VulkanPathTracePass::WriteLayerDescriptorSets(const VulkanPathTraceLayerPass& layer)
{
    for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
    {
        const VkDescriptorSet set = m_layerDescriptorSets.at(readIndex);
        const uint32_t writeIndex = 1u - readIndex;
        const auto sampled = [&](VkImageView view)
        {
            return VkDescriptorImageInfo{m_nearestSampler, view, kReadLayout};
        };
        const auto storage = [](VkImageView view)
        {
            return VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        };
        const auto history = [&](const HistoryImagePair& pair)
        {
            return VkDescriptorImageInfo{m_nearestSampler, pair.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
        };
        // The layer's G-buffer has no coat, specular or sheen (gbuffer.frag clears their flags there),
        // so its surface image stands in for the three, never read.
        const std::array<VkDescriptorImageInfo, kBindingCount> infos = {
            sampled(layer.GetDepthView()),
            sampled(layer.GetNormalView()),
            sampled(layer.GetAlbedoView()),
            sampled(layer.GetSurfaceView()),
            sampled(layer.GetSurfaceView()),
            sampled(layer.GetVelocityView()),
            sampled(layer.GetSurfaceView()),
            sampled(layer.GetSurfaceView()),
            storage(m_raw.GetView(0)),
            storage(m_raw.GetView(1)),
            storage(m_layerDiffuseHistory.GetView(writeIndex)),
            storage(m_layerSpecularHistory.GetView(writeIndex)),
            storage(m_layerResult.GetView(0)),
            storage(m_layerResult.GetView(1)),
            history(m_layerDiffuseHistory),
            history(m_layerSpecularHistory),
            history(m_layerSurfaceHistory),
            storage(m_layerSurfaceHistory.GetView(writeIndex)),
            VkDescriptorImageInfo{m_multiScattering.sampler, m_multiScattering.imageView, VK_IMAGE_LAYOUT_GENERAL}};
        std::array<VkWriteDescriptorSet, kBindingCount> writes{};
        for (uint32_t binding = 0; binding < kBindingCount; ++binding)
        {
            const VkDescriptorType type = infos[binding].sampler != VK_NULL_HANDLE ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[binding] = ImageWrite(set, binding, type, &infos[binding]);
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanPathTracePass::DestroyImages()
{
    m_imagesReady = false;
    m_layerReady = false;
    m_rawReady = false;
    m_raw.Destroy();
    m_diffuseHistory.Destroy();
    m_specularHistory.Destroy();
    m_surfaceHistory.Destroy();
    m_layerDiffuseHistory.Destroy();
    m_layerSpecularHistory.Destroy();
    m_layerSurfaceHistory.Destroy();
    m_layerResult.Destroy();
}

void VulkanPathTracePass::DestroyHandles()
{
    for (VkPipeline* pipeline : {&m_tracePipeline, &m_temporalPipeline, &m_filterPipeline})
    {
        if (*pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }
    for (VkPipelineLayout* layout : {&m_tracePipelineLayout, &m_pipelineLayout})
    {
        if (*layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    m_layerDescriptorSets.clear();
    DestroyImages();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_nearestSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_nearestSampler, nullptr);
        m_nearestSampler = VK_NULL_HANDLE;
    }
}
}
