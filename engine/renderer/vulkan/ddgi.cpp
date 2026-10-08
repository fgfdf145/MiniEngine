#include "ddgi.h"

#include "compute_pass_util.h"
#include "nvrhi_resources.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace me
{

namespace
{
// Irradiance: rgb and the sky visibility. Visibility: the distance's two moments, all it holds, so
// half the memory of the irradiance's format per texel.
constexpr VkFormat kIrradianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kVisibilityFormat = VK_FORMAT_R16G16_SFLOAT;

// Must match DdgiTraceConstants in shaders/vulkan/ddgi_trace.comp and DdgiUpdateConstants in
// ddgi_update.comp, which share the first 48 bytes and the count.
struct DdgiConstants
{
    glm::vec4 rotation[3]{};
    uint32_t scheduledCount = 0;
    // The trace reads the frame index here, the update the hysteresis as float bits.
    uint32_t frameIndexOrHysteresis = 0;
    // The update's lighting epoch (bits 0 to 7) and geometry epoch (bits 8 to 11).
    uint32_t epochs = 0;
    uint32_t padding = 0;
};

// A memory barrier, and with it any image barriers (layout transitions) in the same command.
void GlobalBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStage,
    VkAccessFlags srcAccess,
    VkPipelineStageFlags dstStage,
    VkAccessFlags dstAccess,
    std::span<const VkImageMemoryBarrier> images = {})
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(
        commandBuffer, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, static_cast<uint32_t>(images.size()), images.data());
}
}

VulkanDdgi::VulkanDdgi(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout raySetLayout,
    uint32_t frameCount,
    bool rayQuery)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_frameCount(frameCount)
{
    try
    {
        m_irradiance = CreateAtlas(kDdgiIrradianceTexels, kIrradianceFormat);
        m_visibility = CreateAtlas(kDdgiVisibilityTexels, kVisibilityFormat);
        m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
        m_states = CreateBuffer(
            static_cast<VkDeviceSize>(kDdgiProbeStateBytes) * kDdgiProbesPerLevel * kDdgiMaxLevels,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            false);
        m_rays = CreateBuffer(sizeof(glm::vec4) * kDdgiRaysPerProbe * kMaxProbesPerFrame, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        for (uint32_t slot = 0; slot < m_frameCount; ++slot)
        {
            m_schedules.push_back(CreateBuffer(sizeof(uint32_t) * kMaxProbesPerFrame, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
            m_feedback.push_back(CreateBuffer(sizeof(uint32_t) * kMaxProbesPerFrame, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
        }
        m_scheduleCounts.assign(m_frameCount, 0u);
        m_recordedSchedules.resize(m_frameCount);
        m_feedbackPending.assign(m_frameCount, 0u);

        // The update's sampled views of the atlases and their samplers (6 to 9): it writes one level
        // while it samples the next coarser one, so it reads them in GENERAL, not through set 0.
        constexpr std::array<VkDescriptorType, 10> kTypes = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLER,
            VK_DESCRIPTOR_TYPE_SAMPLER};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        const std::array<VkDescriptorPoolSize, 4> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * m_frameCount},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * m_frameCount},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * m_frameCount},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 2 * m_frameCount}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = m_frameCount;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the DDGI descriptor pool");
        m_sets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, m_frameCount);
        for (uint32_t slot = 0; slot < m_frameCount; ++slot)
        {
            const VkDescriptorBufferInfo scheduleInfo{m_schedules[slot].buffer, 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo raysInfo{m_rays.buffer, 0, VK_WHOLE_SIZE};
            const VkDescriptorImageInfo irradianceInfo{VK_NULL_HANDLE, m_irradiance.view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo visibilityInfo{VK_NULL_HANDLE, m_visibility.view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo statesInfo{m_states.buffer, 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo feedbackInfo{m_feedback[slot].buffer, 0, VK_WHOLE_SIZE};
            const VkDescriptorImageInfo sampledIrradianceInfo{VK_NULL_HANDLE, m_irradiance.view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo sampledVisibilityInfo{VK_NULL_HANDLE, m_visibility.view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo samplerInfo{NativeSampler(m_sampler), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            std::array<VkWriteDescriptorSet, 10> writes{};
            for (uint32_t binding = 0; binding < writes.size(); ++binding)
            {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = m_sets[slot];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = kTypes[binding];
            }
            writes[0].pBufferInfo = &scheduleInfo;
            writes[1].pBufferInfo = &raysInfo;
            writes[2].pImageInfo = &irradianceInfo;
            writes[3].pImageInfo = &visibilityInfo;
            writes[4].pBufferInfo = &statesInfo;
            writes[5].pBufferInfo = &feedbackInfo;
            writes[6].pImageInfo = &sampledIrradianceInfo;
            writes[7].pImageInfo = &sampledVisibilityInfo;
            writes[8].pImageInfo = &samplerInfo;
            writes[9].pImageInfo = &samplerInfo;
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        const std::array<VkDescriptorSetLayout, 3> setLayouts = {frameSetLayout, raySetLayout, m_setLayout};
        CreateComputePipeline(m_device, pipelineCache, setLayouts, "ddgi_trace.comp.spv", sizeof(DdgiConstants), m_pipelineLayout, m_tracePipeline);
        if (rayQuery)
        {
            m_rayQueryTracePipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "ddgi_trace_ray_query.comp.spv");
        }
        m_updatePipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "ddgi_update.comp.spv");
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanDdgi::~VulkanDdgi()
{
    DestroyHandles();
}

void VulkanDdgi::SetSchedule(uint32_t frameSlot, std::span<const uint32_t> probes)
{
    const size_t count = std::min<size_t>(probes.size(), kMaxProbesPerFrame);
    if (count > 0)
    {
        std::memcpy(m_schedules[frameSlot].mapped, probes.data(), sizeof(uint32_t) * count);
    }
    m_scheduleCounts[frameSlot] = static_cast<uint32_t>(count);
    m_recordedSchedules[frameSlot].assign(probes.begin(), probes.begin() + static_cast<std::ptrdiff_t>(count));
    m_feedbackPending[frameSlot] = 0u;
}

void VulkanDdgi::TakeFeedback(uint32_t frameSlot, std::vector<uint32_t>& scheduled, std::vector<uint32_t>& feedback)
{
    scheduled.clear();
    feedback.clear();
    if (m_feedbackPending[frameSlot] == 0u)
    {
        return;
    }
    m_feedbackPending[frameSlot] = 0u;
    scheduled = m_recordedSchedules[frameSlot];
    const uint32_t* reports = static_cast<const uint32_t*>(m_feedback[frameSlot].mapped);
    feedback.assign(reports, reports + scheduled.size());
}

void VulkanDdgi::Invalidate()
{
    m_cleared = false;
}

void VulkanDdgi::Record(
    VkCommandBuffer commandBuffer,
    VkDescriptorSet frameSet,
    VkDescriptorSet raySet,
    uint32_t frameSlot,
    uint32_t frameIndex,
    float hysteresis,
    uint32_t lightingEpoch,
    uint32_t geometryEpoch,
    bool rayQuery)
{
    if (!m_cleared)
    {
        // First use, or new content: every probe black and never updated. Transitioning from
        // UNDEFINED discards whatever the atlases held, which is the point.
        std::array<VkImageMemoryBarrier, 2> barriers{};
        const std::array<VkImage, 2> images = {m_irradiance.image, m_visibility.image};
        for (size_t index = 0; index < barriers.size(); ++index)
        {
            barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[index].image = images[index];
            barriers[index].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kDdgiMaxLevels};
            barriers[index].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barriers[index].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data());
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kDdgiMaxLevels};
        for (VkImage image : images)
        {
            vkCmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        }
        vkCmdFillBuffer(commandBuffer, m_states.buffer, 0, VK_WHOLE_SIZE, 0u);
        GlobalBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        // Where set 0 samples them.
        EndFrameImageWrites(commandBuffer, images, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        m_cleared = true;
    }

    const uint32_t count = m_scheduleCounts[frameSlot];
    if (count == 0)
    {
        return;
    }

    // The previous frame's reads of the atlases and states, and the last update's writes, before
    // this frame's trace reads and update writes.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    DdgiConstants constants{};
    const glm::mat3 rotation = DdgiRayRotation(frameIndex);
    for (int column = 0; column < 3; ++column)
    {
        constants.rotation[column] = glm::vec4(rotation[column], 0.0f);
    }
    constants.scheduledCount = count;
    constants.frameIndexOrHysteresis = frameIndex;

    const std::array<VkDescriptorSet, 3> sets = {frameSet, raySet, m_sets[frameSlot]};
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    const bool useRayQuery = rayQuery && m_rayQueryTracePipeline != VK_NULL_HANDLE;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, useRayQuery ? m_rayQueryTracePipeline : m_tracePipeline);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(commandBuffer, count, 1, 1);

    // The trace's rays before the update reads them, and the atlases, which the trace sampled
    // through set 0 (its infinite bounce) and earlier frames' shading too, to GENERAL for the update.
    const std::array<VkImageMemoryBarrier, 2> toGeneral = {
        ColorImageTransition(m_irradiance.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
        ColorImageTransition(m_visibility.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)};
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        toGeneral);

    std::memcpy(&constants.frameIndexOrHysteresis, &hysteresis, sizeof(hysteresis));
    constants.epochs = (lightingEpoch & 0xffu) | ((geometryEpoch & 0xfu) << 8);
    m_feedbackPending[frameSlot] = 1u;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_updatePipeline);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(commandBuffer, count, 1, 1);

    // The new atlases, back where set 0 samples them, and states before any shading reads them, and
    // the feedback before the CPU reads it once the frame's fence signals.
    const std::array<VkImageMemoryBarrier, 2> toSampled = {
        ColorImageTransition(m_irradiance.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        ColorImageTransition(m_visibility.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)};
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT,
        toSampled);
}

TextureDescriptorBinding VulkanDdgi::GetIrradianceBinding() const
{
    return TextureDescriptorBinding{m_irradiance.view, NativeSampler(m_sampler)};
}

TextureDescriptorBinding VulkanDdgi::GetVisibilityBinding() const
{
    return TextureDescriptorBinding{m_visibility.view, NativeSampler(m_sampler)};
}

VkBuffer VulkanDdgi::GetProbeStateBuffer() const
{
    return m_states.buffer;
}

VkImage VulkanDdgi::GetIrradianceImage() const
{
    return m_irradiance.image;
}

VkImage VulkanDdgi::GetVisibilityImage() const
{
    return m_visibility.image;
}

VulkanDdgi::Image VulkanDdgi::CreateAtlas(uint32_t texelsPerProbe, VkFormat format)
{
    Image atlas{};
    const uint32_t tile = texelsPerProbe + 2;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {tile * static_cast<uint32_t>(kDdgiGridSize.x * kDdgiGridSize.y), tile * static_cast<uint32_t>(kDdgiGridSize.z), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = kDdgiMaxLevels;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Transfer source for the DDGI reference comparison, which reads the probes back.
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    atlas.texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, atlas.image, "Failed to create a DDGI atlas");
    // Assigned as each handle exists, so DestroyHandles releases a partial atlas.
    Image& target = texelsPerProbe == kDdgiIrradianceTexels ? m_irradiance : m_visibility;
    target = atlas;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = target.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kDdgiMaxLevels};
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &target.view), "Failed to create a DDGI atlas view");
    return target;
}

VulkanDdgi::Buffer VulkanDdgi::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible)
{
    Buffer result{};
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result.handle = CreateNvrhiBuffer(m_nvrhiDevice, bufferInfo, hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, result.buffer, "Failed to create a DDGI buffer", hostVisible ? &result.mapped : nullptr);
    return result;
}

void VulkanDdgi::DestroyHandles()
{
    for (VkPipeline* pipeline : {&m_tracePipeline, &m_rayQueryTracePipeline, &m_updatePipeline})
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
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    std::vector<Buffer*> buffers = {&m_states, &m_rays};
    for (Buffer& schedule : m_schedules)
    {
        buffers.push_back(&schedule);
    }
    for (Buffer& feedback : m_feedback)
    {
        buffers.push_back(&feedback);
    }
    for (Buffer* buffer : buffers)
    {
        // The buffer and its memory go with the handle, released with the rest below.
        *buffer = Buffer{};
    }
    m_sampler = nullptr;
    for (Image* image : {&m_irradiance, &m_visibility})
    {
        if (image->view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, image->view, nullptr);
        }
        // The image and its memory go with the texture, released with the rest below.
        *image = Image{};
    }
}
}
