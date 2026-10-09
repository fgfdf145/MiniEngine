#include "restir_pt_pass.h"

#include "compute_pass_util.h"
#include "gpu_timer.h"
#include "nvrhi_pass.h"
#include "ray_scene.h"

#include <engine/renderer/restir_pairing.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace me
{

namespace
{
// Must match RestirPtConstants in shaders/vulkan/restir_pt_common.slang.
struct RestirPtPushConstants
{
    glm::uvec2 extent{0u};
    uint32_t frameIndex = 0;
    uint32_t flags = 0;
    uint32_t maxBounces = 0;
    uint32_t neeCandidates = 0;
    uint32_t debugView = 0;
    uint32_t accumulatedFrames = 0;
    float footprintScale = 0.0f;
    float roughnessThreshold = 0.0f;
    float legacyDistance = 0.0f;
    float cap = 0.0f;
    float capMin = 0.0f;
    float capGamma = 0.0f;
    uint32_t unused0 = 0;
    uint32_t unused1 = 0;
    glm::vec4 previousCamera{0.0f};
};
static_assert(sizeof(RestirPtPushConstants) == 80, "RestirPtPushConstants must match restir_pt_common.slang");

// The PT_FLAG_* constants in restir_pt_common.slang.
constexpr uint32_t kFlagTemporal = 1u;
constexpr uint32_t kFlagSpatial = 2u;
constexpr uint32_t kFlagFootprint = 4u;
constexpr uint32_t kFlagDecorrelation = 8u;
constexpr uint32_t kFlagColorNoise = 16u;
constexpr uint32_t kFlagDualMotion = 32u;
constexpr uint32_t kFlagRussianRoulette = 64u;
constexpr uint32_t kFlagAccumulate = 128u;
constexpr uint32_t kFlagHistoryValid = 256u;
constexpr uint32_t kFlagPermutation = 512u;

// PT_PAIRING_SIZES: three textures of different sizes, so their repeats do not line up (section 3.2),
// each with sigma 16, the Gaussian that matches the usual 30-pixel uniform disk (section 7).
constexpr std::array<uint32_t, 3> kPairingSizes = {254u, 230u, 210u};
constexpr float kPairingSigma = 16.0f;
constexpr uint32_t kSpatialNeighbours = 3;
// Bytes a pixel: the working and history reservoirs, two surface records, the shifts, the duplication
// score and the accumulation. Every record is a uint4 (16 bytes) but the duplication score.
constexpr uint64_t kReservoirBytes = 64;
constexpr uint64_t kSurfaceBytes = 32;
constexpr uint64_t kShiftBytes = 16 * kSpatialNeighbours;
constexpr uint32_t kRecordStride = 16;
constexpr uint32_t kSampledBindings = 6;

void Dispatch(nvrhi::ICommandList* commandList, VkExtent2D extent, uint32_t groupSize)
{
    commandList->dispatch((extent.width + groupSize - 1) / groupSize, (extent.height + groupSize - 1) / groupSize);
}
}

VulkanRestirPtPass::VulkanRestirPtPass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_nvrhiDevice(nvrhiDevice)
{
    (void)targets;
    if (!rayScene.HasHardwareRayTracing())
    {
        return;
    }
    using Item = nvrhi::BindingLayoutItem;
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 2;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    for (uint32_t binding = 0; binding < kSampledBindings; ++binding)
    {
        desc.bindings.push_back(Item::Texture_SRV(binding));
    }
    desc.bindings.push_back(Item::Texture_UAV(6));
    desc.bindings.push_back(Item::StructuredBuffer_UAV(7));
    desc.bindings.push_back(Item::StructuredBuffer_UAV(8));
    desc.bindings.push_back(Item::StructuredBuffer_UAV(9));
    desc.bindings.push_back(Item::StructuredBuffer_SRV(10));
    desc.bindings.push_back(Item::StructuredBuffer_UAV(11));
    desc.bindings.push_back(Item::StructuredBuffer_UAV(12));
    desc.bindings.push_back(Item::StructuredBuffer_SRV(13));
    desc.bindings.push_back(Item::StructuredBuffer_UAV(14));
    desc.bindings.push_back(Item::PushConstants(0, sizeof(RestirPtPushConstants)));
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, desc, "Failed to create the ReSTIR PT binding layout");
    const auto pipeline = [&](const char* shader)
    {
        return CreateNvrhiComputePipeline(
            m_nvrhiDevice, shader, {frameSetLayout, rayScene.GetNvrhiSetLayout(), m_setLayout, rayScene.GetNvrhiTextureSetLayout()});
    };
    m_initialPipeline = pipeline("restir_pt_initial.comp.spv");
    m_temporalPipeline = pipeline("restir_pt_temporal.comp.spv");
    m_spatialShiftPipeline = pipeline("restir_pt_spatial_shift.comp.spv");
    m_spatialPipeline = pipeline("restir_pt_spatial.comp.spv");
    m_duplicationPipeline = pipeline("restir_pt_duplication.comp.spv");
}

VulkanRestirPtPass::~VulkanRestirPtPass() = default;

bool VulkanRestirPtPass::IsAvailable() const
{
    return m_duplicationPipeline != nullptr;
}

bool VulkanRestirPtPass::Prepare(const SceneRenderTargets& targets)
{
    if (!IsAvailable() || m_prepared)
    {
        return false;
    }
    CreateResources(targets);
    return true;
}

ScenePassId VulkanRestirPtPass::Id() const
{
    return ScenePassId::RestirPt;
}

RenderPassIo VulkanRestirPtPass::Io() const
{
    static constexpr std::array<RenderTargetId, 6> kReads = {
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferCoat,
        RenderTargetId::GBufferVelocity};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::ScenePathTrace};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanRestirPtPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    if (!frame.pathTracing.restir || !m_prepared || frame.rayBindingSet == nullptr || frame.rayTextureTable == nullptr)
    {
        return;
    }
    const RestirPtSettings& settings = frame.pathTracing.restirPt;
    RestirPtPushConstants constants{};
    constants.extent = glm::uvec2(frame.extent.width, frame.extent.height);
    constants.frameIndex = frame.frameIndex;
    constants.flags = (settings.temporalReuse ? kFlagTemporal : 0u) | (settings.spatialReuse ? kFlagSpatial : 0u) |
                      (settings.footprintReconnection ? kFlagFootprint : 0u) | (settings.decorrelation ? kFlagDecorrelation : 0u) |
                      (settings.colorNoiseReduction ? kFlagColorNoise : 0u) | (settings.dualMotionVectors ? kFlagDualMotion : 0u) |
                      (settings.russianRoulette ? kFlagRussianRoulette : 0u) | (settings.accumulate ? kFlagAccumulate : 0u) |
                      (frame.restirPtHistory.valid ? kFlagHistoryValid : 0u) | (settings.permutationSampling ? kFlagPermutation : 0u);
    constants.maxBounces = static_cast<uint32_t>(std::clamp(frame.pathTracing.maxBounces, 0, 5));
    constants.neeCandidates = static_cast<uint32_t>(std::clamp(frame.pathTracing.lightCandidates, 1, 32));
    constants.debugView = static_cast<uint32_t>(std::clamp(settings.debugView, 0, 5));
    constants.accumulatedFrames = frame.restirPtAccumulatedFrames;
    constants.footprintScale = std::max(settings.footprintScale, 0.0f);
    constants.roughnessThreshold = std::clamp(settings.roughnessThreshold, 0.0f, 1.0f);
    constants.legacyDistance = std::max(settings.legacyDistance, 0.0f);
    constants.cap = std::max(settings.cap, 1.0f);
    constants.capMin = std::clamp(settings.capMin, 0.0f, constants.cap);
    constants.capGamma = std::max(settings.capGamma, 1e-3f);
    constants.previousCamera = glm::vec4(frame.previousCameraPosition, 1.0f);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::ScenePathTrace, frame.imageIndex, frame.frameSlot);
    const uint32_t writeIndex = frame.restirPtHistory.writeIndex;
    nvrhi::ICommandList* commandList = frame.commandList;
    nvrhi::ITexture* output = targets.GetTexture(RenderTargetId::ScenePathTrace, slot);
    // The G-buffer is where the native passes left it; ScenePathTrace is written in GENERAL. The buffers
    // are this pass's own (NVRHI tracks them): last frame's history and duplication scores reach this
    // frame's reads through their transitions.
    const NvrhiPassScope scope(commandList, {{output, nvrhi::ResourceStates::UnorderedAccess}});
    const std::array<nvrhi::IBuffer*, 6> written = {
        m_workReservoirs, m_historyReservoirs, m_surfaces[writeIndex], m_shifts, m_duplication, m_accumulation};
    const auto storeBarrier = [&]()
    {
        for (nvrhi::IBuffer* buffer : written)
        {
            commandList->setBufferState(buffer, nvrhi::ResourceStates::UnorderedAccess);
        }
        commandList->setTextureState(output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
    };
    commandList->setBufferState(m_surfaces[1u - writeIndex], nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(m_pairing, nvrhi::ResourceStates::ShaderResource);
    storeBarrier();

    nvrhi::ComputeState state;
    state.bindings = {frame.frameBindingSet, frame.rayBindingSet, m_bindingSets.at(slot * 2 + writeIndex), frame.rayTextureTable};
    const auto dispatch = [&](nvrhi::IComputePipeline* pipeline, uint32_t groupSize)
    {
        state.pipeline = pipeline;
        commandList->setComputeState(state);
        commandList->setPushConstants(&constants, sizeof(constants));
        Dispatch(commandList, frame.extent, groupSize);
    };
    // Each dispatch is its own GPU timer section; the renderer's mark after the pass closes the last.
    const auto mark = [&](const char* name)
    {
        if (frame.gpuTimer != nullptr)
        {
            frame.gpuTimer->Mark(commandBuffer, name);
        }
    };
    dispatch(m_initialPipeline, kComputeWorkgroupSize);
    if (!settings.temporalReuse && !settings.spatialReuse)
    {
        return;
    }
    storeBarrier();
    mark("RestirPtInitial");
    if (settings.temporalReuse)
    {
        dispatch(m_temporalPipeline, kComputeWorkgroupSize);
        storeBarrier();
        mark("RestirPtTemporal");
    }
    if (settings.spatialReuse)
    {
        dispatch(m_spatialShiftPipeline, kComputeWorkgroupSize);
        storeBarrier();
        mark("RestirPtSpatialShift");
    }
    dispatch(m_spatialPipeline, kComputeWorkgroupSize);
    mark("RestirPtSpatial");
    if (settings.temporalReuse && settings.decorrelation)
    {
        storeBarrier();
        dispatch(m_duplicationPipeline, 16);
    }
}

void VulkanRestirPtPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the history at the same call sites.
    if (m_prepared)
    {
        CreateResources(targets);
    }
}

void VulkanRestirPtPass::CreateResources(const SceneRenderTargets& targets)
{
    DestroyResources();
    m_extent = targets.GetExtent();
    const uint64_t pixels = static_cast<uint64_t>(std::max(m_extent.width, 1u)) * std::max(m_extent.height, 1u);
    try
    {
        m_workReservoirs = CreateDeviceBuffer(m_nvrhiDevice, pixels * kReservoirBytes, kRecordStride, true, "ReSTIR PT work reservoirs");
        m_historyReservoirs = CreateDeviceBuffer(m_nvrhiDevice, pixels * kReservoirBytes, kRecordStride, true, "ReSTIR PT history reservoirs");
        for (nvrhi::BufferHandle& surfaces : m_surfaces)
        {
            surfaces = CreateDeviceBuffer(m_nvrhiDevice, pixels * kSurfaceBytes, kRecordStride, true, "ReSTIR PT surfaces");
        }
        m_shifts = CreateDeviceBuffer(m_nvrhiDevice, pixels * kShiftBytes, kRecordStride, true, "ReSTIR PT shifts");
        m_duplication = CreateDeviceBuffer(m_nvrhiDevice, pixels * sizeof(float), sizeof(float), true, "ReSTIR PT duplication");
        m_accumulation = CreateDeviceBuffer(m_nvrhiDevice, pixels * 16, 16, true, "ReSTIR PT accumulation");
        // The pairing textures, two signed bytes a texel, in the shader's order.
        std::vector<uint16_t> texels;
        for (size_t index = 0; index < kPairingSizes.size(); ++index)
        {
            for (const PairingOffset& offset : BuildPairingTexture(kPairingSizes[index], kPairingSigma, 0x5eed0000u + static_cast<uint32_t>(index)))
            {
                texels.push_back(static_cast<uint16_t>(static_cast<uint8_t>(offset.dx) | (static_cast<uint8_t>(offset.dy) << 8)));
            }
        }
        if (texels.size() % 2 != 0)
        {
            texels.push_back(0);
        }
        const uint64_t pairingBytes = texels.size() * sizeof(uint16_t);
        void* mapped = nullptr;
        m_pairing = CreateUploadBuffer(m_nvrhiDevice, pairingBytes, sizeof(uint32_t), "ReSTIR PT pairing", &mapped);
        std::memcpy(mapped, texels.data(), pairingBytes);
        m_nvrhiDevice->unmapBuffer(m_pairing);

        static constexpr std::array<RenderTargetId, kSampledBindings> kSampled = {
            RenderTargetId::SceneDepth,
            RenderTargetId::GBufferNormal,
            RenderTargetId::GBufferAlbedo,
            RenderTargetId::GBufferSurface,
            RenderTargetId::GBufferCoat,
            RenderTargetId::GBufferVelocity};
        using Item = nvrhi::BindingSetItem;
        for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
        {
            for (uint32_t writeIndex = 0; writeIndex < 2; ++writeIndex)
            {
                nvrhi::BindingSetDesc desc;
                for (uint32_t binding = 0; binding < kSampledBindings; ++binding)
                {
                    desc.bindings.push_back(Item::Texture_SRV(binding, targets.GetTexture(kSampled[binding], slot)));
                }
                desc.bindings.push_back(Item::Texture_UAV(6, targets.GetTexture(RenderTargetId::ScenePathTrace, slot)));
                desc.bindings.push_back(Item::StructuredBuffer_UAV(7, m_workReservoirs));
                desc.bindings.push_back(Item::StructuredBuffer_UAV(8, m_historyReservoirs));
                desc.bindings.push_back(Item::StructuredBuffer_UAV(9, m_surfaces[writeIndex]));
                desc.bindings.push_back(Item::StructuredBuffer_SRV(10, m_surfaces[1u - writeIndex]));
                desc.bindings.push_back(Item::StructuredBuffer_UAV(11, m_shifts));
                desc.bindings.push_back(Item::StructuredBuffer_UAV(12, m_duplication));
                desc.bindings.push_back(Item::StructuredBuffer_SRV(13, m_pairing));
                desc.bindings.push_back(Item::StructuredBuffer_UAV(14, m_accumulation));
                desc.bindings.push_back(Item::PushConstants(0, sizeof(RestirPtPushConstants)));
                m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a ReSTIR PT binding set"));
            }
        }
        m_prepared = true;
    }
    catch (...)
    {
        DestroyResources();
        throw;
    }
}

void VulkanRestirPtPass::DestroyResources()
{
    m_prepared = false;
    m_bindingSets.clear();
    m_workReservoirs = nullptr;
    m_historyReservoirs = nullptr;
    m_surfaces = {};
    m_shifts = nullptr;
    m_duplication = nullptr;
    m_pairing = nullptr;
    m_accumulation = nullptr;
}
}
