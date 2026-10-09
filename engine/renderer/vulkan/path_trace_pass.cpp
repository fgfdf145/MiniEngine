#include "path_trace_pass.h"

#include "command.h"
#include <engine/renderer/path_tracing.h>
#include "path_trace_lights.h"
#include "gpu_timer.h"
#include "nvrhi_pass.h"
#include "path_trace_layer_pass.h"
#include "ray_scene.h"

#include <algorithm>
#include <array>

namespace me
{

namespace
{
// Must match PathTraceConstants in shaders/vulkan/path_trace_common.slang.
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
    uint32_t gbufferShift = 0;
    uint32_t samplesPerPixel = 1;
    uint32_t emissiveNeeBounces = 1;
    glm::uvec2 padding{0u};
};
static_assert(sizeof(PathTracePushConstants) == 80, "PathTracePushConstants must match path_trace_common.slang");

// Must match the PT_FLAG_* and PT_IMAGE_* constants in path_trace_common.slang.
constexpr uint32_t kFlagAccumulate = 1u;
constexpr uint32_t kFlagDenoise = 2u;
constexpr uint32_t kFlagHistoryValid = 4u;
constexpr uint32_t kFlagRayMedia = 8u;
constexpr uint32_t kFlagForwardSurfaces = 16u;
constexpr uint32_t kFlagEmissiveLights = 32u;
constexpr uint32_t kFlagLightGrid = 64u;
constexpr uint32_t kFlagHitDistance = 128u;
constexpr uint32_t kFlagDirectLight = 256u;
constexpr uint32_t kFlagHold = 512u;
constexpr uint32_t kFlagRegularize = 1024u;
constexpr uint32_t kImageRaw = 0u;
constexpr uint32_t kImageHistory = 1u;
constexpr uint32_t kImageFinal = 2u;

constexpr VkFormat kImageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// The offline mode's two accumulations.
constexpr VkFormat kFullPrecisionFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
// The multiple scattering LUT, the one input the shaders sample (path_trace_common.slang); the
// G-buffer and history inputs are loaded.
constexpr uint32_t kMultiScatteringBinding = 18;
// Bounces and light candidates a path may be given at most.
constexpr int kMaxBounces = 16;
constexpr int kMaxLightCandidates = 32;

// path_trace_common.slang's set at registerSpace: G-buffer 0-7, raw 8-9, histories written 10-11,
// finals 12-13, histories read 14-16, the surface history written 17, the LUT 18.
nvrhi::BindingLayoutHandle CreateSetLayout(nvrhi::IDevice* device, uint32_t registerSpace)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = registerSpace;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    for (uint32_t binding = 0; binding <= kMultiScatteringBinding; ++binding)
    {
        const bool storage = (binding >= 8 && binding <= 13) || binding == 17;
        desc.bindings.push_back(storage ? nvrhi::BindingLayoutItem::Texture_UAV(binding) : nvrhi::BindingLayoutItem::Texture_SRV(binding));
    }
    desc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + kMultiScatteringBinding));
    desc.bindings.push_back(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PathTracePushConstants)));
    return CreateNvrhiBindingLayout(device, desc, "Failed to create the path tracing binding layout");
}

void Dispatch(nvrhi::ICommandList* commandList, VkExtent2D extent)
{
    commandList->dispatch(
        (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize);
}
}

VulkanPathTracePass::VulkanPathTracePass(
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    const VulkanRayScene& rayScene,
    TextureDescriptorBinding multiScattering)
    : m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_multiScattering(multiScattering)
{
    (void)targets;
    if (!rayScene.HasHardwareRayTracing())
    {
        return;
    }
    m_nearestSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
    m_setLayout = CreateSetLayout(m_nvrhiDevice, 1);
    m_traceSetLayout = CreateSetLayout(m_nvrhiDevice, 2);
    m_filterPipelines[0] = CreateNvrhiComputePipeline(m_nvrhiDevice, "path_trace_filter.comp.spv", {frameSetLayout, m_setLayout});
    m_temporalPipelines[0] = CreateNvrhiComputePipeline(m_nvrhiDevice, "path_trace_temporal.comp.spv", {frameSetLayout, m_setLayout});
    m_filterPipelines[1] = CreateNvrhiComputePipeline(m_nvrhiDevice, "path_trace_filter_float32.comp.spv", {frameSetLayout, m_setLayout});
    m_temporalPipelines[1] = CreateNvrhiComputePipeline(m_nvrhiDevice, "path_trace_temporal_float32.comp.spv", {frameSetLayout, m_setLayout});
    m_lights = std::make_unique<VulkanPathTraceLights>(
        m_nvrhiDevice, static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight), frameSetLayout, rayScene);
    const auto trace = [&](const char* shader)
    {
        return CreateNvrhiComputePipeline(
            m_nvrhiDevice,
            shader,
            {frameSetLayout, rayScene.GetNvrhiSetLayout(), m_traceSetLayout, rayScene.GetNvrhiTextureSetLayout(), m_lights->GetTraceLayout()});
    };
    m_tracePipelines[1][1] = trace("path_trace.comp.spv");
    m_tracePipelines[0][1] = trace("path_trace_no_transmission.comp.spv");
    m_tracePipelines[1][0] = trace("path_trace_no_layers.comp.spv");
    m_tracePipelines[0][0] = trace("path_trace_base.comp.spv");
    m_rayScene = &rayScene;
}

VulkanPathTracePass::~VulkanPathTracePass()
{
    DestroyImages();
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
    return m_tracePipelines[1][1] != nullptr;
}

bool VulkanPathTracePass::SetFullPrecisionHistory(bool fullPrecision)
{
    if (fullPrecision == m_fullPrecision)
    {
        return false;
    }
    m_fullPrecision = fullPrecision;
    return m_imagesReady || m_layerReady;
}

void VulkanPathTracePass::ReleaseImages()
{
    DestroyImages();
}

void VulkanPathTracePass::CreateRaw(VkExtent2D extent)
{
    if (!m_rawReady)
    {
        m_raw.Create(m_nvrhiDevice, m_device, extent, kImageFormat);
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
        const VkFormat historyFormat = m_fullPrecision ? kFullPrecisionFormat : kImageFormat;
        m_diffuseHistory.Create(m_nvrhiDevice, m_device, extent, historyFormat);
        m_specularHistory.Create(m_nvrhiDevice, m_device, extent, historyFormat);
        m_surfaceHistory.Create(m_nvrhiDevice, m_device, extent, kImageFormat);
        CreateBindingSets(targets);
    }
    catch (...)
    {
        DestroyImages();
        throw;
    }
    m_imagesReady = true;
    return true;
}

bool VulkanPathTracePass::PrepareLayer(const SceneRenderTargets& targets, const VulkanPathTraceLayerPass& layer, uint32_t shift)
{
    if (m_layerReady || !IsSupported() || !layer.IsReady())
    {
        return false;
    }
    try
    {
        // The raw pair is the opaque trace's too, at the full size; the layer uses its top-left part.
        CreateRaw(targets.GetExtent());
        const VkExtent2D extent{(targets.GetExtent().width + (1u << shift) - 1u) >> shift, (targets.GetExtent().height + (1u << shift) - 1u) >> shift};
        m_layerShift = shift;
        const VkFormat historyFormat = m_fullPrecision ? kFullPrecisionFormat : kImageFormat;
        m_layerDiffuseHistory.Create(m_nvrhiDevice, m_device, extent, historyFormat);
        m_layerSpecularHistory.Create(m_nvrhiDevice, m_device, extent, historyFormat);
        m_layerSurfaceHistory.Create(m_nvrhiDevice, m_device, extent, kImageFormat);
        m_layerResult.Create(m_nvrhiDevice, m_device, extent, kImageFormat);
        CreateLayerBindingSets(layer);
    }
    catch (...)
    {
        DestroyImages();
        throw;
    }
    m_layerReady = true;
    m_layerInitialized = false;
    return true;
}

bool VulkanPathTracePass::IsLayerReady() const
{
    return m_layerReady;
}

uint32_t VulkanPathTracePass::GetLayerShift() const
{
    return m_layerShift;
}

void VulkanPathTracePass::DestroyLayerImages()
{
    m_layerReady = false;
    m_layerTraceSets.clear();
    m_layerSets.clear();
    m_layerDiffuseHistory.Destroy();
    m_layerSpecularHistory.Destroy();
    m_layerSurfaceHistory.Destroy();
    m_layerResult.Destroy();
}

void VulkanPathTracePass::RecordLayerInitialTransition(nvrhi::ICommandList* commandList) const
{
    if (m_layerInitialized || !m_layerReady)
    {
        return;
    }
    // From nothing to where set 0 samples them.
    const NvrhiPassScope scope(
        commandList,
        {{m_layerResult.GetTexture(0), nvrhi::ResourceStates::Common, nvrhi::ResourceStates::ShaderResource},
         {m_layerResult.GetTexture(1), nvrhi::ResourceStates::Common, nvrhi::ResourceStates::ShaderResource}});
    m_layerInitialized = true;
}

TextureDescriptorBinding VulkanPathTracePass::GetLayerDiffuseBinding() const
{
    return BindTexture(m_layerResult.GetView(0), m_layerResult.GetTexture(0), m_nearestSampler);
}

TextureDescriptorBinding VulkanPathTracePass::GetLayerSpecularBinding() const
{
    return BindTexture(m_layerResult.GetView(1), m_layerResult.GetTexture(1), m_nearestSampler);
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
    nvrhi::ICommandList* commandList = frame.commandList;
    const PathTracingSettings& settings = frame.pathTracing;
    // The emissive lights and the light grid both traces below pick from, this frame's.
    if ((settings.emissiveLights || settings.lightGrid) && ((!settings.restir && m_imagesReady) || (frame.pathTraceLayer && m_layerReady)))
    {
        m_lights->Record(
            commandList, frame.frameBindingSet, frame.rayBindingSet, frame.rayTextureTable, frame.frameSlot, frame.frameIndex, settings.emissiveLights,
            settings.lightGrid && settings.lightCandidates > 0, frame.localLightCount);
        if (frame.gpuTimer != nullptr && (m_lights->GetLightCount(frame.frameSlot) > 0 || m_lights->HasLightGrid(frame.frameSlot)))
        {
            frame.gpuTimer->Mark(commandBuffer, "PathTraceLights");
        }
    }
    nvrhi::ITexture* lut = m_multiScattering.texture;
    // ReSTIR PT (restir_pt_pass.h) runs in the plain path tracer's place when pathTracing.restir is set.
    if (!settings.restir && m_imagesReady)
    {
        const bool historyValid = settings.accumulate && frame.pathTraceHistory.valid;
        const bool hold = frame.pathTraceHold && historyValid;
        const uint32_t readIndex = frame.pathTraceHistory.readIndex;
        const uint32_t writeIndex = 1u - readIndex;
        const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneGi, frame.imageIndex, frame.frameSlot);
        const std::array<nvrhi::ITexture*, 2>& finals = m_finalTargets.at(slot * 2 + readIndex);
        // The raw paths are rewritten whole; the histories keep last frame's contents where they are
        // valid. The finals are written in GENERAL, where the native passes expect them; the LUT rests
        // there too.
        const NvrhiPassScope scope(
            commandList,
            {m_raw.Shared(0, false), m_raw.Shared(1, false),
             m_diffuseHistory.Shared(0, historyValid), m_diffuseHistory.Shared(1, historyValid),
             m_specularHistory.Shared(0, historyValid), m_specularHistory.Shared(1, historyValid),
             m_surfaceHistory.Shared(0, historyValid), m_surfaceHistory.Shared(1, historyValid),
             {finals[0], nvrhi::ResourceStates::UnorderedAccess}, {finals[1], nvrhi::ResourceStates::UnorderedAccess},
             {lut, nvrhi::ResourceStates::UnorderedAccess}});
        for (nvrhi::ITexture* read : {m_diffuseHistory.GetTexture(readIndex), m_specularHistory.GetTexture(readIndex), m_surfaceHistory.GetTexture(readIndex), lut})
        {
            commandList->setTextureState(read, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        }
        commandList->commitBarriers();
        const std::array<nvrhi::ITexture*, 7> written = {
            m_raw.GetTexture(0), m_raw.GetTexture(1), m_diffuseHistory.GetTexture(writeIndex), m_specularHistory.GetTexture(writeIndex),
            m_surfaceHistory.GetTexture(writeIndex), finals[0], finals[1]};
        RecordPaths(
            commandList, frame, m_traceSets.at(slot * 2 + readIndex), m_sets.at(slot * 2 + readIndex), written, settings.accumulate, settings.denoise,
            historyValid, frame.pathTraceHistoryScale, frame.pathTraceHitDistance, 0u, settings.offline.enabled, hold);
        // Its own GPU timer section; the renderer's mark after the pass closes the layer's.
        if (frame.pathTraceLayer && m_layerReady && frame.gpuTimer != nullptr)
        {
            frame.gpuTimer->Mark(commandBuffer, "PathTraceOpaque");
        }
    }

    if (frame.pathTraceLayer && m_layerReady)
    {
        RecordLayerInitialTransition(commandList);
        const bool historyValid = frame.pathTraceLayerAccumulate && frame.pathTraceLayerHistory.valid;
        const bool hold = frame.pathTraceHold && historyValid;
        const uint32_t readIndex = frame.pathTraceLayerHistory.readIndex;
        // The raw pair again, after the plain path tracer's last read of it. The result is rewritten
        // whole: from where last frame's forward pass sampled it to the stores, and back once written.
        const NvrhiPassScope scope(
            commandList,
            {m_raw.Shared(0, false), m_raw.Shared(1, false),
             m_layerDiffuseHistory.Shared(0, historyValid), m_layerDiffuseHistory.Shared(1, historyValid),
             m_layerSpecularHistory.Shared(0, historyValid), m_layerSpecularHistory.Shared(1, historyValid),
             m_layerSurfaceHistory.Shared(0, historyValid), m_layerSurfaceHistory.Shared(1, historyValid),
             {m_layerResult.GetTexture(0), nvrhi::ResourceStates::Common, nvrhi::ResourceStates::ShaderResource},
             {m_layerResult.GetTexture(1), nvrhi::ResourceStates::Common, nvrhi::ResourceStates::ShaderResource},
             {lut, nvrhi::ResourceStates::UnorderedAccess}});
        for (nvrhi::ITexture* read :
             {m_layerDiffuseHistory.GetTexture(readIndex), m_layerSpecularHistory.GetTexture(readIndex), m_layerSurfaceHistory.GetTexture(readIndex), lut})
        {
            commandList->setTextureState(read, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        }
        commandList->setTextureState(m_layerResult.GetTexture(0), nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_layerResult.GetTexture(1), nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
        const uint32_t writeIndex = 1u - readIndex;
        const std::array<nvrhi::ITexture*, 7> written = {
            m_raw.GetTexture(0), m_raw.GetTexture(1), m_layerDiffuseHistory.GetTexture(writeIndex), m_layerSpecularHistory.GetTexture(writeIndex),
            m_layerSurfaceHistory.GetTexture(writeIndex), m_layerResult.GetTexture(0), m_layerResult.GetTexture(1)};
        RecordPaths(
            commandList, frame, m_layerTraceSets.at(readIndex), m_layerSets.at(readIndex), written, frame.pathTraceLayerAccumulate,
            frame.pathTraceLayerDenoise, historyValid, frame.pathTraceLayerHistoryScale, false, m_layerShift, false, hold);
    }
}

void VulkanPathTracePass::RecordPaths(
    nvrhi::ICommandList* commandList,
    const ScenePassFrameContext& frame,
    nvrhi::IBindingSet* traceSet,
    nvrhi::IBindingSet* passSet,
    std::span<nvrhi::ITexture* const> written,
    bool accumulate,
    bool denoise,
    bool historyValid,
    float historyScale,
    bool hitDistance,
    uint32_t gbufferShift,
    bool directLight,
    bool hold) const
{
    const PathTracingSettings& settings = frame.pathTracing;
    // The grid traced: the frame's extent, or 2^gbufferShift times smaller each way (rounded up).
    const VkExtent2D extent{
        (frame.extent.width + (1u << gbufferShift) - 1u) >> gbufferShift, (frame.extent.height + (1u << gbufferShift) - 1u) >> gbufferShift};
    PathTracePushConstants constants{};
    constants.gbufferShift = gbufferShift;
    constants.extent = glm::vec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.frameIndex = frame.frameIndex;
    constants.flags = (accumulate ? kFlagAccumulate : 0u) | (denoise ? kFlagDenoise : 0u) | (historyValid ? kFlagHistoryValid : 0u) |
                      (settings.rayMedia ? kFlagRayMedia : 0u) | (settings.forwardSurfaces ? kFlagForwardSurfaces : 0u) |
                      (settings.emissiveLights && m_lights->GetLightCount(frame.frameSlot) > 0 ? kFlagEmissiveLights : 0u) |
                      (m_lights->HasLightGrid(frame.frameSlot) ? kFlagLightGrid : 0u) | (hitDistance ? kFlagHitDistance : 0u) |
                      (directLight ? kFlagDirectLight : 0u) | (hold ? kFlagHold : 0u);
    // Path regularization comes with the firefly clamp in real time; the offline mode has a switch.
    const bool regularize = settings.offline.enabled ? settings.offline.pathRegularization : settings.fireflyClamp > 0.0f;
    constants.flags |= regularize ? kFlagRegularize : 0u;
    constants.maxBounces = static_cast<uint32_t>(std::clamp(settings.maxBounces, 0, kMaxBounces));
    constants.lightCandidates = static_cast<uint32_t>(std::clamp(settings.lightCandidates, 0, kMaxLightCandidates));
    constants.fireflyClamp = std::max(settings.fireflyClamp, 0.0f);
    constants.historyScale = historyScale;
    constants.historyCap = static_cast<float>(std::max(frame.pathTraceHistoryCap, 1u));
    constants.motionFrames = static_cast<float>(std::max(settings.motionFrames, 1));
    // The offline mode's samples, and its emissive next event estimation at every vertex; the real-time
    // one looks for emissive triangles at the first path vertex only, where they matter most.
    constants.samplesPerPixel = settings.offline.enabled ? OfflineSamplesPerPixel(settings.offline) : 1u;
    constants.emissiveNeeBounces = settings.offline.enabled ? constants.maxBounces : 1u;

    // Between dispatches: every image they store to, ordered (a UAV barrier each).
    const auto storeBarrier = [&]()
    {
        for (nvrhi::ITexture* texture : written)
        {
            commandList->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }
        commandList->commitBarriers();
    };
    // The stored images out of wherever the scope left them (nothing, for history it discards).
    storeBarrier();
    // Transmissive surfaces are met whole only with the forward surfaces on (otherwise by coverage, as
    // opaque); layered hits only where some material has layers.
    const bool transmission = settings.forwardSurfaces;
    const bool layered = m_rayScene == nullptr || m_rayScene->HasLayeredMaterials();
    // A held image traces nothing: the temporal pass carries its accumulation over.
    if (!hold)
    {
        nvrhi::ComputeState state;
        state.pipeline = m_tracePipelines[transmission ? 1 : 0][layered ? 1 : 0];
        state.bindings = {frame.frameBindingSet, frame.rayBindingSet, traceSet, frame.rayTextureTable, m_lights->GetTraceSet(frame.frameSlot)};
        commandList->setComputeState(state);
        commandList->setPushConstants(&constants, sizeof(constants));
        Dispatch(commandList, extent);
        if (!accumulate && !denoise)
        {
            return;
        }
        storeBarrier();
    }

    nvrhi::ComputeState state;
    state.bindings = {frame.frameBindingSet, passSet};
    if (accumulate)
    {
        state.pipeline = m_temporalPipelines[m_fullPrecision ? 1 : 0];
        commandList->setComputeState(state);
        commandList->setPushConstants(&constants, sizeof(constants));
        Dispatch(commandList, extent);
        storeBarrier();
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
    state.pipeline = m_filterPipelines[m_fullPrecision ? 1 : 0];
    for (size_t index = 0; index < iterations.size(); ++index)
    {
        const Iteration& iteration = iterations[index];
        if (index > 0)
        {
            storeBarrier();
        }
        // Set again after each barrier: NVRHI's commitBarriers does not unbind, but a clear state elsewhere would.
        commandList->setComputeState(state);
        constants.stepSize = iteration.stepSize;
        constants.source = iteration.source;
        constants.target = iteration.target;
        commandList->setPushConstants(&constants, sizeof(constants));
        Dispatch(commandList, extent);
    }
}

void VulkanPathTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // Remade at the new size by the next path traced frame's Prepare, which then resets the history.
    (void)targets;
    DestroyImages();
}

nvrhi::BindingSetDesc VulkanPathTracePass::DescribeSet(
    const Inputs& inputs,
    const HistoryImagePair& diffuse,
    const HistoryImagePair& specular,
    const HistoryImagePair& surface,
    uint32_t readIndex) const
{
    const uint32_t writeIndex = 1u - readIndex;
    using Item = nvrhi::BindingSetItem;
    nvrhi::BindingSetDesc desc;
    for (uint32_t binding = 0; binding < 8; ++binding)
    {
        desc.bindings.push_back(Item::Texture_SRV(binding, inputs.gbuffer[binding]));
    }
    desc.bindings.push_back(Item::Texture_UAV(8, m_raw.GetTexture(0)));
    desc.bindings.push_back(Item::Texture_UAV(9, m_raw.GetTexture(1)));
    desc.bindings.push_back(Item::Texture_UAV(10, diffuse.GetTexture(writeIndex)));
    desc.bindings.push_back(Item::Texture_UAV(11, specular.GetTexture(writeIndex)));
    desc.bindings.push_back(Item::Texture_UAV(12, inputs.final[0]));
    desc.bindings.push_back(Item::Texture_UAV(13, inputs.final[1]));
    desc.bindings.push_back(Item::Texture_SRV(14, diffuse.GetTexture(readIndex)));
    desc.bindings.push_back(Item::Texture_SRV(15, specular.GetTexture(readIndex)));
    desc.bindings.push_back(Item::Texture_SRV(16, surface.GetTexture(readIndex)));
    desc.bindings.push_back(Item::Texture_UAV(17, surface.GetTexture(writeIndex)));
    desc.bindings.push_back(Item::Texture_SRV(kMultiScatteringBinding, m_multiScattering.texture));
    desc.bindings.push_back(Item::Sampler(kSplitSamplerBindingOffset + kMultiScatteringBinding, m_multiScattering.nvrhiSampler));
    desc.bindings.push_back(Item::PushConstants(0, sizeof(PathTracePushConstants)));
    return desc;
}

void VulkanPathTracePass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_traceSets.clear();
    m_sets.clear();
    m_finalTargets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        Inputs inputs;
        inputs.gbuffer = {
            targets.GetTexture(RenderTargetId::SceneDepth, slot),
            targets.GetTexture(RenderTargetId::GBufferNormal, slot),
            targets.GetTexture(RenderTargetId::GBufferAlbedo, slot),
            targets.GetTexture(RenderTargetId::GBufferSurface, slot),
            targets.GetTexture(RenderTargetId::GBufferCoat, slot),
            targets.GetTexture(RenderTargetId::GBufferVelocity, slot),
            targets.GetTexture(RenderTargetId::GBufferSpecular, slot),
            targets.GetTexture(RenderTargetId::GBufferSheen, slot)};
        inputs.final[0] = targets.GetTexture(RenderTargetId::SceneGi, slot);
        inputs.final[1] = targets.GetTexture(RenderTargetId::SceneReflections, slot);
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const nvrhi::BindingSetDesc desc = DescribeSet(inputs, m_diffuseHistory, m_specularHistory, m_surfaceHistory, readIndex);
            m_traceSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_traceSetLayout, "Failed to create a path tracing binding set"));
            m_sets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a path tracing binding set"));
            m_finalTargets.push_back({inputs.final[0], inputs.final[1]});
        }
    }
}

void VulkanPathTracePass::CreateLayerBindingSets(const VulkanPathTraceLayerPass& layer)
{
    m_layerTraceSets.clear();
    m_layerSets.clear();
    // The layer's G-buffer has no coat, specular or sheen (gbuffer.frag clears their flags there), so
    // its surface image stands in for the three, never read.
    Inputs inputs;
    inputs.gbuffer = {
        layer.GetDepthTexture(),
        layer.GetNormalTexture(),
        layer.GetAlbedoTexture(),
        layer.GetSurfaceTexture(),
        layer.GetSurfaceTexture(),
        layer.GetVelocityTexture(),
        layer.GetSurfaceTexture(),
        layer.GetSurfaceTexture()};
    inputs.final[0] = m_layerResult.GetTexture(0);
    inputs.final[1] = m_layerResult.GetTexture(1);
    for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
    {
        const nvrhi::BindingSetDesc desc = DescribeSet(inputs, m_layerDiffuseHistory, m_layerSpecularHistory, m_layerSurfaceHistory, readIndex);
        m_layerTraceSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_traceSetLayout, "Failed to create a path tracing binding set"));
        m_layerSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a path tracing binding set"));
    }
}

void VulkanPathTracePass::DestroyImages()
{
    m_imagesReady = false;
    m_layerReady = false;
    m_rawReady = false;
    m_traceSets.clear();
    m_sets.clear();
    m_finalTargets.clear();
    m_layerTraceSets.clear();
    m_layerSets.clear();
    m_raw.Destroy();
    m_diffuseHistory.Destroy();
    m_specularHistory.Destroy();
    m_surfaceHistory.Destroy();
    m_layerDiffuseHistory.Destroy();
    m_layerSpecularHistory.Destroy();
    m_layerSurfaceHistory.Destroy();
    m_layerResult.Destroy();
}
}
