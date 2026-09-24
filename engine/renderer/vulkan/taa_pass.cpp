#include "taa_pass.h"

#include <array>

namespace me
{

namespace
{
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match TaaConstants in shaders/vulkan/taa_resolve.comp.
struct TaaPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    float exposure = 1.0f;
    uint32_t flags = 0;
    glm::vec2 unused{0.0f};
};
static_assert(sizeof(TaaPushConstants) == 32, "TaaPushConstants must match taa_resolve.comp");

// Must match the TAA_FLAG_* constants in taa_resolve.comp.
constexpr uint32_t kFlagEnabled = 1u;
constexpr uint32_t kFlagHistoryValid = 2u;
}

VulkanTaaPass::VulkanTaaPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        m_nearestSampler = CreateClampSampler(m_device, VK_FILTER_NEAREST);
        m_linearSampler = CreateClampSampler(m_device, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, 6> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        CreateComputePipeline(
            m_device,
            pipelineCache,
            frameSetLayout,
            m_setLayout,
            "taa_resolve.comp.spv",
            sizeof(TaaPushConstants),
            m_pipelineLayout,
            m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 4, 2);
        m_history.Create(m_physicalDevice, m_device, targets.GetExtent(), kHistoryFormat);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTaaPass::~VulkanTaaPass()
{
    DestroyHandles();
}

ScenePassId VulkanTaaPass::Id() const
{
    return ScenePassId::Taa;
}

RenderPassIo VulkanTaaPass::Io() const
{
    // The velocity target is declared in both orders because the bound set names it; in the
    // forward-only order it was never written, and the pass, passing through, never samples it.
    static constexpr std::array<RenderTargetId, 3> kReads = {
        RenderTargetId::SceneHdr,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferVelocity};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneTaa};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanTaaPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    m_history.RecordBarrier(commandBuffer, frame.taaHistory.valid);

    TaaPushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.exposure = frame.exposure;
    constants.flags = (frame.taaEnabled ? kFlagEnabled : 0u) | (frame.taaHistory.valid ? kFlagHistoryValid : 0u);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot * 2 + frame.taaHistory.readIndex),
        &constants,
        sizeof(constants),
        frame.extent);
}

void VulkanTaaPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the TAA TemporalHistory at the same call sites, so the next frame
    // discards the new images' undefined contents.
    m_history.Create(m_physicalDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateDescriptorSets(targets);
}

void VulkanTaaPass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount * 2);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const VkDescriptorSet set = m_descriptorSets[slot * 2 + readIndex];
            const VkDescriptorImageInfo currentInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::SceneHdr, slot), kReadLayout};
            const VkDescriptorImageInfo depthInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo historyReadInfo{m_linearSampler, m_history.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historyWriteInfo{VK_NULL_HANDLE, m_history.GetView(1u - readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo outputInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneTaa, slot), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 6> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &currentInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &velocityInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &historyReadInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outputInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void VulkanTaaPass::DestroyHandles()
{
    if (m_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    m_history.Destroy();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    for (VkSampler* sampler : {&m_nearestSampler, &m_linearSampler})
    {
        if (*sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, *sampler, nullptr);
            *sampler = VK_NULL_HANDLE;
        }
    }
}
}
