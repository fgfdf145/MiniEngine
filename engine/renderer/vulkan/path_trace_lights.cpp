#include "path_trace_lights.h"

#include "compute_pass_util.h"
#include "nvrhi_resources.h"
#include "ray_scene.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace me
{

namespace
{
// Must match EmissiveBuildConstants in shaders/vulkan/emissive_lights.comp.
struct EmissiveBuildConstants
{
    uint32_t mode = 0;
    uint32_t count = 0;
    uint32_t level = 0;
    uint32_t depth = 0;
};

constexpr uint32_t kBindingCount = 6;
// Must match the LIGHT_GRID_* constants in shaders/vulkan/light_grid_common.glsl.
constexpr uint32_t kLightGridCells = 64u * 16u * 64u;
constexpr uint32_t kLightGridSlots = 32u;
constexpr VkDeviceSize kLightGridBytes = VkDeviceSize{kLightGridCells} * (1u + kLightGridSlots / 2u) * sizeof(uint32_t);
constexpr uint32_t kWorkgroupSize = 64;
// EmissiveTriangle in emissive_lights_common.glsl.
constexpr VkDeviceSize kTriangleBytes = 96;

// EmissiveTreeLevelStart in emissive_lights_common.glsl: floats before level `level`.
uint32_t TreeLevelStart(uint32_t level)
{
    return 4u + ((1u << (2u * level)) - 4u) / 3u;
}

void ComputeBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags sourceStage, VkAccessFlags sourceAccess)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, sourceStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

uint32_t Groups(uint32_t count)
{
    return (count + kWorkgroupSize - 1) / kWorkgroupSize;
}
}

VulkanPathTraceLights::VulkanPathTraceLights(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    uint32_t frameCount,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_rayScene(rayScene)
{
    try
    {
        std::array<VkDescriptorType, kBindingCount> types{};
        types.fill(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_setLayout = CreateComputeSetLayout(m_device, types);
        const std::array<VkDescriptorSetLayout, 4> layouts = {
            frameSetLayout, rayScene.GetSetLayout(), m_setLayout, rayScene.GetTextureSetLayout()};
        CreateComputePipeline(
            m_device, pipelineCache, layouts, "emissive_lights.comp.spv", sizeof(EmissiveBuildConstants), m_pipelineLayout, m_pipeline);
        m_gridPipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "light_grid.comp.spv");

        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindingCount * frameCount};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = frameCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the emissive light pool");
        const std::vector<VkDescriptorSet> sets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, frameCount);
        m_slots.resize(frameCount);
        for (uint32_t index = 0; index < frameCount; ++index)
        {
            Slot& slot = m_slots[index];
            slot.set = sets[index];
            // Placeholders, so the set is valid before the first light.
            slot.triangles = CreateBuffer(kTriangleBytes, false);
            slot.tree = CreateBuffer(sizeof(float) * TreeLevelStart(2), false);
            slot.slotInstances = CreateBuffer(sizeof(uint32_t), false);
            slot.slotBases = CreateBuffer(sizeof(uint32_t), true);
            slot.entries = CreateBuffer(sizeof(uint32_t) * 2, true);
            slot.grid = CreateBuffer(sizeof(uint32_t), false);
            WriteSet(slot);
        }
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanPathTraceLights::~VulkanPathTraceLights()
{
    DestroyHandles();
}

VkDescriptorSetLayout VulkanPathTraceLights::GetSetLayout() const
{
    return m_setLayout;
}

VkDescriptorSet VulkanPathTraceLights::GetSet(uint32_t frameSlot) const
{
    return m_slots.at(frameSlot).set;
}

uint32_t VulkanPathTraceLights::GetLightCount(uint32_t frameSlot) const
{
    return frameSlot < m_slots.size() && m_slots[frameSlot].emissiveBuilt ? m_slots[frameSlot].lightCount : 0u;
}

bool VulkanPathTraceLights::HasLightGrid(uint32_t frameSlot) const
{
    return frameSlot < m_slots.size() && m_slots[frameSlot].gridBuilt;
}

VulkanPathTraceLights::Buffer VulkanPathTraceLights::CreateBuffer(VkDeviceSize size, bool hostVisible) const
{
    Buffer result{};
    result.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = result.size;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result.handle = CreateNvrhiBuffer(m_nvrhiDevice, bufferInfo, hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, result.buffer, "Failed to create an emissive light buffer", hostVisible ? &result.mapped : nullptr);
    return result;
}

void VulkanPathTraceLights::DestroyBuffer(Buffer& buffer) const
{
    // The buffer and its memory go with the handle, released with the rest below.
    buffer = Buffer{};
}

bool VulkanPathTraceLights::EnsureBuffer(Buffer& buffer, VkDeviceSize size, bool hostVisible) const
{
    if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= size)
    {
        return false;
    }
    // A quarter more, so a streamed map's changes do not remake it every time.
    const VkDeviceSize grown = size + size / 4;
    DestroyBuffer(buffer);
    buffer = CreateBuffer(grown, hostVisible);
    return true;
}

void VulkanPathTraceLights::UpdateSlot(Slot& slot)
{
    if (slot.valid && slot.generation == m_rayScene.GetEmissiveGeneration())
    {
        return;
    }
    slot.generation = m_rayScene.GetEmissiveGeneration();
    slot.valid = true;
    const std::vector<RayEmissiveSubmesh>& submeshes = m_rayScene.GetEmissiveSubmeshes();
    const uint32_t slotCapacity = std::max(m_rayScene.GetSlotCapacity(), 1u);
    uint32_t count = 0;
    uint64_t requested = 0;
    for (const RayEmissiveSubmesh& submesh : submeshes)
    {
        count += std::min(submesh.triangleCount, kMaxLights - count);
        requested += submesh.triangleCount;
    }
    bool rewrite = EnsureBuffer(slot.slotBases, sizeof(uint32_t) * slotCapacity, true);
    rewrite |= EnsureBuffer(slot.slotInstances, sizeof(uint32_t) * slotCapacity, false);
    rewrite |= EnsureBuffer(slot.entries, sizeof(uint32_t) * 2 * std::max(count, 1u), true);
    rewrite |= EnsureBuffer(slot.triangles, kTriangleBytes * std::max(count, 1u), false);
    uint32_t depth = 1;
    while ((1u << (2u * depth)) < count)
    {
        ++depth;
    }
    rewrite |= EnsureBuffer(slot.tree, sizeof(float) * TreeLevelStart(depth + 1), false);
    if (rewrite)
    {
        WriteSet(slot);
    }

    auto* bases = static_cast<uint32_t*>(slot.slotBases.mapped);
    std::fill(bases, bases + slot.slotBases.size / sizeof(uint32_t), ~0u);
    auto* entries = static_cast<uint32_t*>(slot.entries.mapped);
    uint32_t written = 0;
    for (const RayEmissiveSubmesh& submesh : submeshes)
    {
        const uint32_t take = std::min(submesh.triangleCount, kMaxLights - written);
        if (take == 0)
        {
            break;
        }
        if (submesh.slot < slotCapacity)
        {
            bases[submesh.slot] = written;
        }
        for (uint32_t triangle = 0; triangle < take; ++triangle)
        {
            entries[2 * (written + triangle)] = submesh.slot;
            entries[2 * (written + triangle) + 1] = triangle;
        }
        written += take;
    }
    if (requested > written)
    {
        LOG_WARN("Emissive lights: {} of {} emissive triangles kept (at most {})", written, requested, kMaxLights);
    }
    slot.lightCount = written;
    slot.depth = depth;
}

void VulkanPathTraceLights::Record(
    VkCommandBuffer commandBuffer,
    VkDescriptorSet frameSet,
    VkDescriptorSet raySet,
    VkDescriptorSet rayTextureSet,
    uint32_t frameSlot,
    uint32_t frameIndex,
    bool emissive,
    bool lightGrid,
    uint32_t localLightCount)
{
    if (frameSlot >= m_slots.size())
    {
        return;
    }
    Slot& slot = m_slots[frameSlot];
    slot.gridBuilt = false;
    slot.emissiveBuilt = false;
    if (emissive)
    {
        UpdateSlot(slot);
    }
    const uint32_t lightCount = emissive ? slot.lightCount : 0u;
    lightGrid = lightGrid && localLightCount > 0;
    if (lightGrid && !slot.gridMade)
    {
        DestroyBuffer(slot.grid);
        slot.grid = CreateBuffer(kLightGridBytes, false);
        slot.gridMade = true;
        WriteSet(slot);
    }
    if (lightCount == 0 && !lightGrid)
    {
        return;
    }
    const std::array<VkDescriptorSet, 4> sets = {frameSet, raySet, slot.set, rayTextureSet};
    if (lightGrid)
    {
        // The grid reads nothing another dispatch here writes.
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_gridPipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        const EmissiveBuildConstants constants{3u, localLightCount, frameIndex, 0u};
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commandBuffer, Groups(kLightGridCells), 1, 1);
        slot.gridBuilt = true;
        if (lightCount == 0)
        {
            ComputeBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            return;
        }
    }
    // Which instance draws each slot is found again every frame; the slots no instance draws stay ~0.
    vkCmdFillBuffer(commandBuffer, slot.slotInstances.buffer, 0, VK_WHOLE_SIZE, ~0u);
    ComputeBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    const auto dispatch = [&](uint32_t mode, uint32_t count, uint32_t level, uint32_t threads)
    {
        const EmissiveBuildConstants constants{mode, count, level, slot.depth};
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commandBuffer, Groups(threads), 1, 1);
        ComputeBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    };
    const uint32_t instances = std::max(m_rayScene.GetInstanceCount(), 1u);
    dispatch(0u, instances, 0u, instances);
    dispatch(1u, slot.lightCount, 0u, 1u << (2u * slot.depth));
    for (uint32_t level = slot.depth; level-- > 0;)
    {
        dispatch(2u, 0u, level, 1u << (2u * level));
    }
    slot.emissiveBuilt = true;
}

void VulkanPathTraceLights::WriteSet(const Slot& slot) const
{
    const std::array<VkDescriptorBufferInfo, kBindingCount> infos = {
        VkDescriptorBufferInfo{slot.triangles.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{slot.tree.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{slot.slotBases.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{slot.slotInstances.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{slot.entries.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{slot.grid.buffer, 0, VK_WHOLE_SIZE}};
    std::array<VkWriteDescriptorSet, kBindingCount> writes{};
    for (uint32_t binding = 0; binding < kBindingCount; ++binding)
    {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = slot.set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VulkanPathTraceLights::DestroyHandles()
{
    for (Slot& slot : m_slots)
    {
        DestroyBuffer(slot.triangles);
        DestroyBuffer(slot.tree);
        DestroyBuffer(slot.slotInstances);
        DestroyBuffer(slot.slotBases);
        DestroyBuffer(slot.entries);
        DestroyBuffer(slot.grid);
    }
    m_slots.clear();
    for (VkPipeline* pipeline : {&m_pipeline, &m_gridPipeline})
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
}
}
