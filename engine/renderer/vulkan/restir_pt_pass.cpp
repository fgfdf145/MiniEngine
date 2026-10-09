#include "restir_pt_pass.h"

#include "gpu_timer.h"
#include "nvrhi_resources.h"
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
// score and the accumulation.
constexpr VkDeviceSize kReservoirBytes = 64;
constexpr VkDeviceSize kSurfaceBytes = 32;
constexpr VkDeviceSize kShiftBytes = 16 * kSpatialNeighbours;

constexpr uint32_t kSampledBindings = 6;
constexpr uint32_t kBufferBindings = 8;
constexpr uint32_t kBindingCount = kSampledBindings + 1 + kBufferBindings;

void ComputeBarrier(VkCommandBuffer commandBuffer)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void Dispatch(VkCommandBuffer commandBuffer, VkExtent2D extent, uint32_t groupSize)
{
    vkCmdDispatch(commandBuffer, (extent.width + groupSize - 1) / groupSize, (extent.height + groupSize - 1) / groupSize, 1);
}
}

VulkanRestirPtPass::VulkanRestirPtPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    (void)targets;
    if (!rayScene.HasHardwareRayTracing())
    {
        return;
    }
    try
    {
        std::array<VkDescriptorType, kBindingCount> types{};
        for (uint32_t binding = 0; binding < kSampledBindings; ++binding)
        {
            types[binding] = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        types[kSampledBindings] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        for (uint32_t binding = kSampledBindings + 1; binding < kBindingCount; ++binding)
        {
            types[binding] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        m_setLayout = CreateComputeSetLayout(m_device, types);
        const std::array<VkDescriptorSetLayout, 4> setLayouts = {
            frameSetLayout, rayScene.GetSetLayout(), m_setLayout, rayScene.GetTextureSetLayout()};
        CreateComputePipeline(
            m_device, pipelineCache, setLayouts, "restir_pt_initial.comp.spv", sizeof(RestirPtPushConstants), m_pipelineLayout, m_initialPipeline);
        m_temporalPipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "restir_pt_temporal.comp.spv");
        m_spatialShiftPipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "restir_pt_spatial_shift.comp.spv");
        m_spatialPipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "restir_pt_spatial.comp.spv");
        m_duplicationPipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "restir_pt_duplication.comp.spv");
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanRestirPtPass::~VulkanRestirPtPass()
{
    DestroyHandles();
}

bool VulkanRestirPtPass::IsAvailable() const
{
    return m_duplicationPipeline != VK_NULL_HANDLE;
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
    if (!frame.pathTracing.restir || !m_prepared || frame.raySet == VK_NULL_HANDLE || frame.rayTextureSet == VK_NULL_HANDLE)
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
    const VkDescriptorSet passSet = m_descriptorSets.at(slot * 2 + frame.restirPtHistory.writeIndex);
    const std::array<VkDescriptorSet, 4> sets = {frame.frameDescriptorSet, frame.raySet, passSet, frame.rayTextureSet};
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);

    // Last frame's history and duplication scores before anything reads or overwrites them.
    ComputeBarrier(commandBuffer);
    // Each dispatch is its own GPU timer section; the renderer's mark after the pass closes the last.
    const auto mark = [&](const char* name)
    {
        if (frame.gpuTimer != nullptr)
        {
            frame.gpuTimer->Mark(commandBuffer, name);
        }
    };
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_initialPipeline);
    Dispatch(commandBuffer, frame.extent, kComputeWorkgroupSize);
    if (!settings.temporalReuse && !settings.spatialReuse)
    {
        return;
    }
    ComputeBarrier(commandBuffer);
    mark("RestirPtInitial");
    if (settings.temporalReuse)
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipeline);
        Dispatch(commandBuffer, frame.extent, kComputeWorkgroupSize);
        ComputeBarrier(commandBuffer);
        mark("RestirPtTemporal");
    }
    if (settings.spatialReuse)
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spatialShiftPipeline);
        Dispatch(commandBuffer, frame.extent, kComputeWorkgroupSize);
        ComputeBarrier(commandBuffer);
        mark("RestirPtSpatialShift");
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spatialPipeline);
    Dispatch(commandBuffer, frame.extent, kComputeWorkgroupSize);
    mark("RestirPtSpatial");
    if (settings.temporalReuse && settings.decorrelation)
    {
        ComputeBarrier(commandBuffer);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_duplicationPipeline);
        Dispatch(commandBuffer, frame.extent, 16);
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

VulkanRestirPtPass::Buffer VulkanRestirPtPass::CreateBuffer(VkDeviceSize size, bool hostVisible) const
{
    Buffer result{};
    result.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = result.size;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result.handle = CreateNvrhiBuffer(m_nvrhiDevice, bufferInfo, hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, result.buffer, "Failed to create a ReSTIR PT buffer");
    return result;
}

void VulkanRestirPtPass::DestroyBuffer(Buffer& buffer) const
{
    // The buffer and its memory go with the handle, released with the rest below.
    buffer = Buffer{};
}

void VulkanRestirPtPass::CreateResources(const SceneRenderTargets& targets)
{
    DestroyResources();
    m_extent = targets.GetExtent();
    const VkDeviceSize pixels = static_cast<VkDeviceSize>(std::max(m_extent.width, 1u)) * std::max(m_extent.height, 1u);
    try
    {
        m_workReservoirs = CreateBuffer(pixels * kReservoirBytes, false);
        m_historyReservoirs = CreateBuffer(pixels * kReservoirBytes, false);
        for (Buffer& surfaces : m_surfaces)
        {
            surfaces = CreateBuffer(pixels * kSurfaceBytes, false);
        }
        m_shifts = CreateBuffer(pixels * kShiftBytes, false);
        m_duplication = CreateBuffer(pixels * sizeof(float), false);
        m_accumulation = CreateBuffer(pixels * 16, false);

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
        const VkDeviceSize pairingBytes = texels.size() * sizeof(uint16_t);
        m_pairing = CreateBuffer(pairingBytes, true);
        void* mapped = m_nvrhiDevice->mapBuffer(m_pairing.handle, nvrhi::CpuAccessMode::Write);
        if (mapped == nullptr)
        {
            throw VulkanError(VK_ERROR_MEMORY_MAP_FAILED, "Failed to map the ReSTIR PT pairing buffer");
        }
        std::memcpy(mapped, texels.data(), pairingBytes);
        m_nvrhiDevice->unmapBuffer(m_pairing.handle);

        const uint32_t copyCount = targets.GetTransientCopyCount();
        const uint32_t setCount = copyCount * 2;
        const std::array<VkDescriptorPoolSize, 3> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, setCount * kSampledBindings},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setCount},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * kBufferBindings}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = setCount;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the ReSTIR PT descriptor pool");
        m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, setCount);

        static constexpr std::array<RenderTargetId, kSampledBindings> kSampled = {
            RenderTargetId::SceneDepth,
            RenderTargetId::GBufferNormal,
            RenderTargetId::GBufferAlbedo,
            RenderTargetId::GBufferSurface,
            RenderTargetId::GBufferCoat,
            RenderTargetId::GBufferVelocity};
        for (uint32_t slot = 0; slot < copyCount; ++slot)
        {
            for (uint32_t writeIndex = 0; writeIndex < 2; ++writeIndex)
            {
                const VkDescriptorSet set = m_descriptorSets[slot * 2 + writeIndex];
                std::array<VkDescriptorImageInfo, kSampledBindings + 1> images{};
                std::array<VkWriteDescriptorSet, kBindingCount> writes{};
                for (uint32_t binding = 0; binding < kSampledBindings; ++binding)
                {
                    images[binding] = VkDescriptorImageInfo{VK_NULL_HANDLE, targets.GetSampledView(kSampled[binding], slot), kReadLayout};
                    writes[binding] = ImageWrite(set, binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &images[binding]);
                }
                images[kSampledBindings] =
                    VkDescriptorImageInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::ScenePathTrace, slot), VK_IMAGE_LAYOUT_GENERAL};
                writes[kSampledBindings] = ImageWrite(set, kSampledBindings, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[kSampledBindings]);

                const std::array<const Buffer*, kBufferBindings> buffers = {
                    &m_workReservoirs,
                    &m_historyReservoirs,
                    &m_surfaces[writeIndex],
                    &m_surfaces[1u - writeIndex],
                    &m_shifts,
                    &m_duplication,
                    &m_pairing,
                    &m_accumulation};
                std::array<VkDescriptorBufferInfo, kBufferBindings> bufferInfos{};
                for (uint32_t index = 0; index < kBufferBindings; ++index)
                {
                    const uint32_t binding = kSampledBindings + 1 + index;
                    bufferInfos[index] = VkDescriptorBufferInfo{buffers[index]->buffer, 0, VK_WHOLE_SIZE};
                    VkWriteDescriptorSet& write = writes[binding];
                    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    write.dstSet = set;
                    write.dstBinding = binding;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    write.pBufferInfo = &bufferInfos[index];
                }
                vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
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
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    for (Buffer* buffer : {&m_workReservoirs, &m_historyReservoirs, &m_surfaces[0], &m_surfaces[1], &m_shifts, &m_duplication, &m_pairing, &m_accumulation})
    {
        DestroyBuffer(*buffer);
    }
}

void VulkanRestirPtPass::DestroyHandles()
{
    DestroyResources();
    for (VkPipeline* pipeline : {&m_initialPipeline, &m_temporalPipeline, &m_spatialShiftPipeline, &m_spatialPipeline, &m_duplicationPipeline})
    {
        if (*pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
}
}
